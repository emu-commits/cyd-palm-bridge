/* graffiti.c -- $1 unistroke recognizer for Graffiti-style text entry (U6).
 *
 * Standard $1 pipeline: resample to N points, rotate to the indicative angle,
 * scale to a reference box, translate to origin, then nearest-template by average
 * point distance (golden-section angle search). Templates are coarse polylines
 * ($1 resamples them). This is a FRAMEWORK: the template set is a starter and the
 * accept threshold needs tuning against the real resistive touch on-device.
 */
#include "graffiti.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "esp_log.h"

#define N        48          /* resample count */
#define MAXPTS   256
#define SQSIZE   100.0f
#define ANGLE_R  (0.15f)     /* +-~8.5 deg; small, so it doesn't help wrong templates fit */
#define PHI      0.6180339887f
#define TAP_PX   16.0f       /* a stroke whose bbox is smaller than this both ways = a tap */
#define LET_THRESH 32.0f     /* accept distance for letters/digits (noisy resistive strip) */
#define PNC_THRESH 34.0f     /* punctuation is a smaller, curvier set -> a touch looser */

typedef struct { float x, y; } Pt;

static Pt s_buf[MAXPTS];
static int s_n;
static int s_punct;          /* punctuation shift armed: next stroke is punctuation */
static float s_last_dist = 1e9f;   /* $1 distance of the last letter/digit match */

/* Per-device user templates (the trainer's "training mode"): the user's own stroke
 * for a letter, captured downsampled to <=GU_PTS raw points. When present, it's
 * matched alongside the built-in template and wins when closer -- calibrating
 * recognition to this exact hand + resistive panel. ~26*32*2*2 = ~3.3 KB. */
#define GU_PTS 32
static int16_t s_user[26][GU_PTS][2];
static uint8_t s_user_n[26];        /* point count per letter, 0 = none */
#define GU_MAGIC 0x47550001u        /* 'GU' v1 */

float graffiti_last_distance(void){ return s_last_dist; }

void graffiti_clear(void){ s_n = 0; }
void graffiti_add_point(int x, int y){
    if(s_n < MAXPTS){ s_buf[s_n].x = (float)x; s_buf[s_n].y = (float)y; s_n++; }
}

/* ---- $1 helpers ---- */
static float path_len(const Pt *p, int n){
    float d=0; for(int i=1;i<n;i++){ float dx=p[i].x-p[i-1].x, dy=p[i].y-p[i-1].y; d+=sqrtf(dx*dx+dy*dy); } return d;
}
static void resample(const Pt *in, int n, Pt *out){
    float I = path_len(in,n) / (N-1);
    int o=0; out[o++]=in[0];
    if(I<=0){ while(o<N){ out[o]=out[o-1]; o++; } return; }
    Pt prev=in[0]; float D=0;
    /* NB: when we emit a point mid-segment we do NOT advance i -- we keep
     * subdividing the SAME segment. A long segment must yield many points; the
     * old code emitted only one per segment, collapsing sparse templates. */
    for(int i=1;i<n && o<N;){
        float dx=in[i].x-prev.x, dy=in[i].y-prev.y, d=sqrtf(dx*dx+dy*dy);
        if(d<=0){ i++; continue; }
        if(D + d >= I){
            float t=(I-D)/d;
            Pt q = { prev.x + t*dx, prev.y + t*dy };
            out[o++]=q; prev=q; D=0;         /* stay on segment i */
        } else { D+=d; prev=in[i]; i++; }    /* consume segment i */
    }
    while(o<N){ out[o]=out[o-1]; o++; }
}
static void centroid(const Pt *p, Pt *c){
    float sx=0,sy=0; for(int i=0;i<N;i++){ sx+=p[i].x; sy+=p[i].y; } c->x=sx/N; c->y=sy/N;
}
static void rotate_by(Pt *p, float rad){
    Pt c; centroid(p,&c); float co=cosf(rad), si=sinf(rad);
    for(int i=0;i<N;i++){ float dx=p[i].x-c.x, dy=p[i].y-c.y;
        p[i].x=dx*co-dy*si+c.x; p[i].y=dx*si+dy*co+c.y; }
}
static void normalize(Pt *p){
    /* NOTE: deliberately NOT rotation-invariant. Graffiti characters are
     * orientation-dependent (a '4' is not a rotated anything), so we keep the
     * drawn orientation and let dist_best() tolerate only a small tilt. */
    float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
    for(int i=0;i<N;i++){
        if(p[i].x<minx)minx=p[i].x;
        if(p[i].x>maxx)maxx=p[i].x;
        if(p[i].y<miny)miny=p[i].y;
        if(p[i].y>maxy)maxy=p[i].y;
    }
    float w=maxx-minx, h=maxy-miny;
    /* UNIFORM scale by the larger dimension so a line stays a line (non-uniform
     * scaling would stretch a thin stroke across the whole box and destroy it). */
    float s = w>h ? w : h; if(s<1) s=1;
    for(int i=0;i<N;i++){ p[i].x=(p[i].x-minx)*SQSIZE/s; p[i].y=(p[i].y-miny)*SQSIZE/s; }
    /* translate centroid to origin */
    Pt c; centroid(p,&c);
    for(int i=0;i<N;i++){ p[i].x-=c.x; p[i].y-=c.y; }
}
static float dist_at(const Pt *a, Pt *b, float rad){
    Pt tmp[N]; memcpy(tmp,b,sizeof tmp); rotate_by(tmp, rad);
    float d=0; for(int i=0;i<N;i++){ float dx=a[i].x-tmp[i].x, dy=a[i].y-tmp[i].y; d+=sqrtf(dx*dx+dy*dy); }
    return d/N;
}
static float dist_best(const Pt *a, Pt *b){
    float lo=-ANGLE_R, hi=ANGLE_R;
    float x1=PHI*lo+(1-PHI)*hi, f1=dist_at(a,b,x1);
    float x2=(1-PHI)*lo+PHI*hi, f2=dist_at(a,b,x2);
    for(int it=0; it<8; it++){
        if(f1<f2){ hi=x2; x2=x1; f2=f1; x1=PHI*lo+(1-PHI)*hi; f1=dist_at(a,b,x1); }
        else     { lo=x1; x1=x2; f1=f2; x2=(1-PHI)*lo+PHI*hi; f2=dist_at(a,b,x2); }
    }
    return f1<f2?f1:f2;
}

/* ---- full Graffiti letter alphabet (single stroke, grid ~0..10, y down).
 * Output is lowercase; the shift upstroke capitalizes. Strokes are drawn like
 * the classic Palm Graffiti capitals; the first point is the pen-down start, so
 * DRAW DIRECTION matters (the recognizer is not rotation-invariant). Coarse but
 * complete -- refine per-letter with the on-device telemetry / training game. ---- */
typedef struct { char c; const float *pts; int n; } Tmpl;
#define T(name,ch,...) static const float name[]={__VA_ARGS__};
T(t_a,'a', 0,10, 5,0, 10,10)                                   /* caret ^ */
T(t_b,'b', 3,10, 3,0, 7,2, 3,5, 7,8, 3,10)                     /* up, then 2 right bumps */
T(t_c,'c', 9,2, 4,1, 1,5, 4,9, 9,8)                            /* open-right C */
T(t_d,'d', 3,10, 3,0, 8,3, 8,7, 3,10)                          /* up, then right bowl */
T(t_e,'e', 8,1, 4,2, 7,5, 4,8, 8,9)                            /* reverse 3 */
T(t_f,'f', 8,1, 3,1, 3,10)                                     /* top bar + down */
T(t_g,'g', 8,3, 4,1, 1,5, 3,9, 7,9, 8,6, 5,6)               /* capital G: C sweep + inward crossbar */
T(t_h,'h', 2,0, 2,10, 2,6, 5,4, 8,6, 8,10)                    /* stem + rounded n-hump */
T(t_i,'i', 5,0, 5,10)                                          /* vertical */
T(t_j,'j', 7,0, 7,8, 5,10, 2,9)                               /* J hook */
T(t_k,'k', 2,0, 2,10, 2,6, 8,2, 4,6, 8,10)                    /* stem + 2 diagonals */
T(t_l,'l', 2,0, 2,10, 8,10)                                   /* right angle */
T(t_m,'m', 1,10, 3,1, 5,6, 7,1, 9,10)                         /* upside-down W */
T(t_n,'n', 1,10, 1,0, 9,10, 9,0)                              /* up, diagonal, up (N) */
T(t_o,'o', 8,2, 4,1, 1,4, 2,8, 6,10, 9,7, 8,3, 4,1)          /* circle */
T(t_p,'p', 2,10, 2,0, 6,0, 8,2, 6,4, 2,4)                    /* long stem + compact top bowl */
T(t_q,'q', 7,1, 4,1, 2,3, 4,5, 7,5, 8,3, 7,1, 7,9)          /* top circle + straight tail */
T(t_r,'r', 2,10, 2,0, 7,2, 3,5, 8,10)                        /* stem, bowl, leg to BR */
T(t_s,'s', 8,3, 5,1, 2,3, 4,5, 8,7, 5,9, 2,8)               /* proportional S: two even lobes */
T(t_t,'t', 2,1, 8,1, 8,10)                                   /* along top, then down */
T(t_u,'u', 1,0, 1,8, 5,10, 9,8, 9,0)                         /* U */
T(t_v,'v', 0,0, 5,10, 10,0)                                  /* V */
T(t_w,'w', 0,0, 3,10, 5,4, 7,10, 10,0)                       /* W */
T(t_x,'x', 1,1, 9,9, 5,5, 9,1, 1,9)                          /* cross (2-stroke; see backlog) */
T(t_y,'y', 2,0, 5,6, 8,0, 5,6, 4,10)                         /* Y w/ tail */
T(t_z,'z', 0,0, 9,0, 0,9, 9,9)                               /* Z */
#undef T
static const Tmpl LTMPL[] = {
    {'a',t_a,3},{'b',t_b,6},{'c',t_c,5},{'d',t_d,5},{'e',t_e,5},{'f',t_f,3},
    {'g',t_g,7},{'h',t_h,6},{'i',t_i,2},{'j',t_j,4},{'k',t_k,6},{'l',t_l,3},
    {'m',t_m,5},{'n',t_n,4},{'o',t_o,8},{'p',t_p,6},{'q',t_q,8},{'r',t_r,5},
    {'s',t_s,7},{'t',t_t,3},{'u',t_u,5},{'v',t_v,3},{'w',t_w,5},{'x',t_x,5},
    {'y',t_y,5},{'z',t_z,4},
};
#define NLTMPL ((int)(sizeof(LTMPL)/sizeof(LTMPL[0])))

/* ---- starter digit set (right "123" pad). Coarse; tune on-device. ---- */
#define T(name,ch,...) static const float name[]={__VA_ARGS__};
T(d_0,'0', 8,1, 2,3, 1,7, 5,10, 9,7, 8,3, 5,1)        /* 0: circle */
T(d_1,'1', 5,0, 5,10)                                  /* 1: down */
T(d_2,'2', 1,2, 4,0, 8,2, 5,6, 1,10, 9,10)             /* 2 */
T(d_3,'3', 1,1, 8,3, 4,5, 8,7, 1,10)                   /* 3 */
T(d_4,'4', 7,0, 1,7, 9,7, 7,4, 7,10)                   /* 4 */
T(d_5,'5', 8,1, 2,1, 2,5, 7,5, 8,8, 3,10)              /* 5 */
T(d_6,'6', 8,1, 3,3, 1,7, 5,10, 8,7, 4,5, 2,6)         /* 6 */
T(d_7,'7', 1,1, 9,1, 4,10)                             /* 7 */
T(d_8,'8', 5,1, 2,3, 5,5, 8,7, 5,9, 2,7, 5,5, 8,3)     /* 8 */
T(d_9,'9', 8,6, 4,5, 2,3, 5,1, 8,3, 8,7, 5,10)         /* 9 */
#undef T
static const Tmpl DTMPL[] = {
    {'0',d_0,7},{'1',d_1,2},{'2',d_2,6},{'3',d_3,5},{'4',d_4,5},
    {'5',d_5,6},{'6',d_6,7},{'7',d_7,3},{'8',d_8,8},{'9',d_9,7},
};
#define NDTMPL ((int)(sizeof(DTMPL)/sizeof(DTMPL[0])))

/* ---- punctuation set (drawn AFTER the punctuation shift = a single tap) -------
 * PalmOS enters punctuation via a punct-shift then one stroke; period is the tap
 * that follows the shift (two taps). The $1 normalizer scales every stroke to a
 * unit box, so only SHAPE survives -- straight lines of the same orientation are
 * indistinguishable, hence at most one horizontal (`-`), one vertical (`'`), and
 * one of each diagonal live here. `_` is shape-identical to `-` (both horizontal)
 * so it is intentionally omitted. Coarse starter, like the letters: tune on-device
 * from the `graf pnc` telemetry. Draw direction matters (not rotation-invariant). */
#define T(name,ch,...) static const float name[]={__VA_ARGS__};
T(p_at,  '@', 8,3, 5,1, 2,4, 3,7, 6,8, 7,5, 5,4)   /* CCW spiral (a-like) */
T(p_com, ',', 5,2, 5,6, 3,9)                        /* down, then hook to lower-left */
T(p_sla, '/', 2,9, 8,1)                             /* diagonal, up to the right */
T(p_dsh, '-', 2,5, 8,5)                             /* horizontal, left to right */
T(p_apo, '\'',5,1, 5,8)                             /* short vertical, top down */
T(p_lp,  '(', 7,1, 3,5, 7,9)                        /* left paren (open right) */
T(p_rp,  ')', 3,1, 7,5, 3,9)                        /* right paren (open left) */
T(p_qm,  '?', 2,3, 5,1, 8,4, 5,7)                   /* question hook */
#undef T
static const Tmpl PTMPL[] = {
    {'@',p_at,7},{',',p_com,3},{'/',p_sla,2},{'-',p_dsh,2},
    {'\'',p_apo,2},{'(',p_lp,3},{')',p_rp,3},{'?',p_qm,4},
};
#define NPTMPL ((int)(sizeof(PTMPL)/sizeof(PTMPL[0])))

/* horizontal swipe -> space (L->R) / backspace (R->L), else 0 */
static char gesture(void){
    if(s_n < 2) return 0;
    float minx=s_buf[0].x,maxx=minx,miny=s_buf[0].y,maxy=miny;
    for(int i=1;i<s_n;i++){
        if(s_buf[i].x<minx)minx=s_buf[i].x;
        if(s_buf[i].x>maxx)maxx=s_buf[i].x;
        if(s_buf[i].y<miny)miny=s_buf[i].y;
        if(s_buf[i].y>maxy)maxy=s_buf[i].y;
    }
    float w=maxx-minx, h=maxy-miny;
    if(w > 24.0f && w > 2.5f*h){                /* long & flat = swipe */
        float dx = s_buf[s_n-1].x - s_buf[0].x;
        return dx >= 0 ? ' ' : '\b';
    }
    if(h > 24.0f && h > 2.5f*w){                /* tall & narrow = vertical stroke */
        float dy = s_buf[s_n-1].y - s_buf[0].y;
        if(dy < -0.6f*h) return GRAF_SHIFT;     /* bottom-to-top = shift (letters go top-down) */
    }
    /* straight diagonal, top-right -> bottom-left = Enter/return. Straightness
     * (path ~ endpoint distance) keeps curved letters like s/z from matching. */
    {
        float dx = s_buf[s_n-1].x - s_buf[0].x;
        float dy = s_buf[s_n-1].y - s_buf[0].y;
        float dist = sqrtf(dx*dx + dy*dy);
        if(dist > 28.0f && dx < -18.0f && dy > 18.0f && path_len(s_buf,s_n) < 1.4f*dist)
            return '\n';
    }
    return 0;
}

/* raw bounding box of the captured stroke (pre-normalize). Returns 0 if empty. */
static int stroke_bbox(float *w, float *h){
    if(s_n < 1){ *w=*h=0; return 0; }
    float x0=s_buf[0].x,x1=x0,y0=s_buf[0].y,y1=y0;
    for(int i=1;i<s_n;i++){
        if(s_buf[i].x<x0) x0=s_buf[i].x;
        if(s_buf[i].x>x1) x1=s_buf[i].x;
        if(s_buf[i].y<y0) y0=s_buf[i].y;
        if(s_buf[i].y>y1) y1=s_buf[i].y;
    }
    *w=x1-x0; *h=y1-y0; return 1;
}

/* $1 nearest-template match of the captured stroke against `set`; returns the
 * best character if within `thresh`, else 0. `label` names the set in telemetry. */
static char match_set(const Tmpl *set, int nset, float thresh, const char *label){
    int npts = s_n;
    float rw=0, rh=0; stroke_bbox(&rw,&rh);
    Pt cand[N]; resample(s_buf, s_n, cand); normalize(cand);
    float best = 1e9f, second = 1e9f; char bc = 0, sc = 0;
    for(int i=0;i<nset;i++){
        Pt tp[MAXPTS];
        for(int k=0;k<set[i].n;k++){ tp[k].x=set[i].pts[2*k]; tp[k].y=set[i].pts[2*k+1]; }
        Pt rt[N]; resample(tp, set[i].n, rt); normalize(rt);
        float d = dist_best(cand, rt);
        if(d < best){ second=best; sc=bc; best=d; bc=set[i].c; }
        else if(d < second){ second=d; sc=set[i].c; }
    }
    /* per-stroke telemetry for on-device tuning (watch `idf.py monitor`) */
    ESP_LOGI("graf","%s pts=%d bbox=%.0fx%.0f -> '%c' d=%.1f (2nd '%c' %.1f)%s",
             label, npts, rw, rh, bc?bc:'?', best, sc?sc:'?', second,
             best<thresh?"":"  [rejected]");
    s_last_dist = best;
    return (best < thresh) ? bc : 0;
}

/* match the buffered stroke against the stored USER templates; best letter + its
 * distance in *out_d (1e9 if none). Same $1 pipeline as match_set. */
static char match_user(float *out_d){
    *out_d = 1e9f;
    Pt cand[N]; resample(s_buf, s_n, cand); normalize(cand);
    float best = 1e9f; char bc = 0;
    for(int i=0;i<26;i++){
        if(!s_user_n[i]) continue;
        Pt tp[GU_PTS];
        for(int k=0;k<s_user_n[i];k++){ tp[k].x=s_user[i][k][0]; tp[k].y=s_user[i][k][1]; }
        Pt rt[N]; resample(tp, s_user_n[i], rt); normalize(rt);
        float d = dist_best(cand, rt);
        if(d < best){ best = d; bc = (char)('a'+i); }
    }
    *out_d = best;
    return bc;
}

/* trainer "training mode": store the just-drawn stroke as the user's template for
 * lowercase letter c (downsampled to GU_PTS points). 1 on success. */
int graffiti_capture_user(char c){
    if(c < 'a' || c > 'z' || s_n < 2) return 0;
    float w=0,h=0; stroke_bbox(&w,&h);
    if(w < TAP_PX && h < TAP_PX) return 0;      /* a tap isn't a letter */
    int idx = c - 'a', out = 0;
    for(int k=0; k<GU_PTS; k++){
        int src = (GU_PTS==1) ? 0 : (int)((long)k * (s_n-1) / (GU_PTS-1));
        if(src >= s_n) src = s_n-1;
        s_user[idx][out][0] = (int16_t)s_buf[src].x;
        s_user[idx][out][1] = (int16_t)s_buf[src].y;
        out++;
        if(out >= s_n) break;              /* short stroke: don't pad past its length */
    }
    s_user_n[idx] = (uint8_t)out;
    return 1;
}
int  graffiti_user_count(void){ int n=0; for(int i=0;i<26;i++) if(s_user_n[i]) n++; return n; }
void graffiti_user_reset(void){ memset(s_user_n,0,sizeof s_user_n); }

int graffiti_user_save(const char *path){
    FILE *f = fopen(path, "wb"); if(!f) return 0;
    uint32_t magic = GU_MAGIC;
    int ok = fwrite(&magic,4,1,f)==1
          && fwrite(s_user_n,1,26,f)==26
          && fwrite(s_user,1,sizeof s_user,f)==1;
    fclose(f); return ok;
}
int graffiti_user_load(const char *path){
    FILE *f = fopen(path, "rb"); if(!f) return 0;
    uint32_t magic=0;
    int ok = fread(&magic,4,1,f)==1 && magic==GU_MAGIC
          && fread(s_user_n,1,26,f)==26
          && fread(s_user,1,sizeof s_user,f)==1;
    fclose(f);
    if(!ok) graffiti_user_reset();
    else for(int i=0;i<26;i++) if(s_user_n[i]>GU_PTS) s_user_n[i]=0;   /* sanitise */
    return ok;
}

char graffiti_recognize(int digits){
    /* Punctuation shift armed (a prior tap): this stroke is punctuation, and a tap
     * is the period. The swipe/backspace gestures do NOT apply while armed -- that
     * is what lets '-' (a horizontal stroke) through instead of reading as a space. */
    if(s_punct){
        s_punct = 0;
        float w=0,h=0; int have = stroke_bbox(&w,&h);
        if(have && w < TAP_PX && h < TAP_PX){ graffiti_clear(); return '.'; }
        char pc = (s_n >= 4) ? match_set(PTMPL, NPTMPL, PNC_THRESH, "pnc") : 0;
        graffiti_clear();
        return pc;
    }

    char g = gesture();
    if(g){ graffiti_clear(); return g; }

    /* a single tap arms the punctuation shift for the next stroke (PalmOS) */
    { float w=0,h=0;
      if(stroke_bbox(&w,&h) && w < TAP_PX && h < TAP_PX){
          s_punct = 1; graffiti_clear(); return GRAF_PUNCT;
      } }

    if(s_n < 4){ graffiti_clear(); return 0; } /* too short: ignore */
    char c = match_set(digits ? DTMPL : LTMPL, digits ? NDTMPL : NLTMPL,
                       LET_THRESH, digits ? "123" : "abc");
    if(!digits){                       /* let a closer USER template win (calibration) */
        float ud; char uc = match_user(&ud);
        if(uc && ud < LET_THRESH && (c==0 || ud < s_last_dist)){ c = uc; s_last_dist = ud; }
    }
    graffiti_clear();
    return c;
}

/* runtime accessor: the ideal stroke for a lowercase letter a-z (control points,
 * grid ~0..10, y down), so the Graffiti trainer can draw a "how to write it"
 * guide. Returns NULL for non-letters. */
const float *graffiti_letter_template(char c, int *npairs){
    if(c < 'a' || c > 'z') return NULL;
    const Tmpl *t = &LTMPL[c - 'a'];
    if(npairs) *npairs = t->n;
    return t->pts;
}

/* runtime accessor for ANY trainable glyph (letter, digit, or punctuation): the
 * ideal stroke as control points (grid ~0..10, y down). Covers a-z, 0-9 and the
 * punctuation set. Returns NULL for glyphs with no drawn stroke (e.g. '.', the tap
 * that follows the punctuation shift). Used by the trainer to draw its guide. */
const float *graffiti_glyph_template(char c, int *npairs){
    const Tmpl *set = NULL; int n = 0;
    if(c>='a' && c<='z'){ set=LTMPL; n=NLTMPL; }
    else if(c>='0' && c<='9'){ set=DTMPL; n=NDTMPL; }
    else { set=PTMPL; n=NPTMPL; }
    for(int i=0;i<n;i++) if(set[i].c==c){ if(npairs)*npairs=set[i].n; return set[i].pts; }
    return NULL;
}

#ifdef GRAF_TEST_HOOKS
/* test-only accessors so the offline accuracy harness (sim/tests/graf_test.c)
 * can synthesize noisy strokes from the REAL templates and measure recognition
 * without duplicating the coordinate tables. which: 0=letters 1=digits 2=punct. */
int graf_test_ntmpl(int which){ return which==0?NLTMPL : which==1?NDTMPL : NPTMPL; }
const float *graf_test_tmpl(int which, int i, char *ch, int *npairs){
    const Tmpl *s = which==0?LTMPL : which==1?DTMPL : PTMPL;
    *ch = s[i].c; *npairs = s[i].n; return s[i].pts;
}
#endif
