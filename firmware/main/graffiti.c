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

#define N        48          /* resample count */
#define MAXPTS   256
#define SQSIZE   100.0f
#define ANGLE_R  (0.25f)     /* +-~14.3 deg search range */
#define PHI      0.6180339887f

typedef struct { float x, y; } Pt;

static Pt s_buf[MAXPTS];
static int s_n;

void graffiti_clear(void){ s_n = 0; }
void graffiti_add_point(int x, int y){
    if(s_n < MAXPTS){ s_buf[s_n].x = (float)x; s_buf[s_n].y = (float)y; s_n++; }
}

/* ---- $1 helpers ---- */
static float path_len(const Pt *p, int n){
    float d=0; for(int i=1;i<n;i++){ float dx=p[i].x-p[i-1].x, dy=p[i].y-p[i-1].y; d+=sqrtf(dx*dx+dy*dy); } return d;
}
static void resample(const Pt *in, int n, Pt *out){
    float I = path_len(in,n) / (N-1); float D=0; int o=0;
    out[o++] = in[0];
    Pt prev = in[0];
    for(int i=1;i<n;i++){
        float dx=in[i].x-prev.x, dy=in[i].y-prev.y, d=sqrtf(dx*dx+dy*dy);
        if(D + d >= I && d > 0){
            float t=(I-D)/d;
            Pt q = { prev.x + t*dx, prev.y + t*dy };
            if(o<N) out[o++]=q;
            prev=q; D=0;
        } else { D+=d; prev=in[i]; }
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
    /* rotate to indicative angle (centroid -> first point at 0) */
    Pt c; centroid(p,&c);
    float ang = atan2f(c.y-p[0].y, c.x-p[0].x);
    rotate_by(p, -ang);
    /* scale to square */
    float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
    for(int i=0;i<N;i++){
        if(p[i].x<minx)minx=p[i].x;
        if(p[i].x>maxx)maxx=p[i].x;
        if(p[i].y<miny)miny=p[i].y;
        if(p[i].y>maxy)maxy=p[i].y;
    }
    float w=maxx-minx, h=maxy-miny;
    if(w<1)w=1;
    if(h<1)h=1;
    for(int i=0;i<N;i++){ p[i].x=(p[i].x-minx)*SQSIZE/w; p[i].y=(p[i].y-miny)*SQSIZE/h; }
    /* translate centroid to origin */
    centroid(p,&c);
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

/* ---- starter template set (coarse polylines; grid ~0..10). Tune on-device. ---- */
typedef struct { char c; const float *pts; int n; } Tmpl;
#define T(name,ch,...) static const float name[]={__VA_ARGS__};
T(t_i,'i', 5,0, 5,10)                                  /* I: straight down */
T(t_l,'l', 3,0, 3,10, 11,10)                           /* L: down, right */
T(t_c,'c', 10,1, 3,3, 1,7, 8,10)                       /* C: left-open curve */
T(t_u,'u', 1,0, 1,8, 5,10, 9,8, 9,0)                   /* U */
T(t_v,'v', 0,0, 5,10, 10,0)                            /* V */
T(t_n,'n', 0,10, 0,0, 9,10, 9,0)                       /* N */
T(t_o,'o', 8,1, 2,3, 1,7, 5,10, 9,7, 8,3, 5,1)         /* O: circle */
T(t_s,'s', 9,1, 3,2, 8,5, 3,8, 8,10)                   /* S */
T(t_r,'r', 0,10, 0,0, 8,3, 0,5)                        /* R-ish */
T(t_t,'t', 0,2, 9,2, 5,2, 5,10)                        /* T-ish cross+down */
T(t_z,'z', 0,0, 9,0, 0,9, 9,9)                         /* Z */
T(t_dot,'.', 4,8, 5,8, 6,9)                            /* tap-ish dot */
#undef T
static const Tmpl TMPL[] = {
    {'i',t_i,2},{'l',t_l,3},{'c',t_c,4},{'u',t_u,5},{'v',t_v,3},{'n',t_n,4},
    {'o',t_o,7},{'s',t_s,5},{'r',t_r,4},{'t',t_t,4},{'z',t_z,4},{'.',t_dot,3},
};
#define NTMPL ((int)(sizeof(TMPL)/sizeof(TMPL[0])))

char graffiti_recognize(void){
    if(s_n < 6) return 0;                       /* too short: ignore */
    Pt cand[N]; resample(s_buf, s_n, cand); normalize(cand);
    float best = 1e9f; char bc = 0;
    for(int i=0;i<NTMPL;i++){
        Pt tp[MAXPTS];
        for(int k=0;k<TMPL[i].n;k++){ tp[k].x=TMPL[i].pts[2*k]; tp[k].y=TMPL[i].pts[2*k+1]; }
        Pt rt[N]; resample(tp, TMPL[i].n, rt); normalize(rt);
        float d = dist_best(cand, rt);
        if(d < best){ best = d; bc = TMPL[i].c; }
    }
    graffiti_clear();
    /* accept threshold (avg normalized point distance). Tune on-device. */
    return (best < 22.0f) ? bc : 0;
}
