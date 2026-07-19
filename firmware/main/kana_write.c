/* kana_write.c -- $1 per-stroke matcher (see kana_write.h). */
#include "kana_write.h"
#include <math.h>
#include <string.h>

#define KW_N       32          /* resample count (a stroke is simpler than a letter) */
#define KW_SQ      100.0f
#define KW_ANGLE   0.15f       /* +-~8.5 deg; keeps drawn orientation (direction matters) */
#define KW_PHI     0.6180339887f

typedef struct { float x, y; } Pt;

static float kw_pathlen(const Pt *p, int n){
    float d=0; for(int i=1;i<n;i++){ float dx=p[i].x-p[i-1].x, dy=p[i].y-p[i-1].y; d+=sqrtf(dx*dx+dy*dy); }
    return d;
}
static void kw_resample(const Pt *in, int n, Pt *out){
    float I = kw_pathlen(in,n) / (KW_N-1);
    int o=0; out[o++]=in[0];
    if(I<=0){ while(o<KW_N){ out[o]=out[o-1]; o++; } return; }
    Pt prev=in[0]; float D=0;
    for(int i=1;i<n && o<KW_N;){
        float dx=in[i].x-prev.x, dy=in[i].y-prev.y, d=sqrtf(dx*dx+dy*dy);
        if(d<=0){ i++; continue; }
        if(D+d>=I){ float t=(I-D)/d; Pt q={prev.x+t*dx, prev.y+t*dy}; out[o++]=q; prev=q; D=0; }
        else { D+=d; prev=in[i]; i++; }
    }
    while(o<KW_N){ out[o]=out[o-1]; o++; }
}
static void kw_centroid(const Pt *p, Pt *c){
    float sx=0,sy=0; for(int i=0;i<KW_N;i++){ sx+=p[i].x; sy+=p[i].y; } c->x=sx/KW_N; c->y=sy/KW_N;
}
static void kw_normalize(Pt *p){
    float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
    for(int i=0;i<KW_N;i++){
        if(p[i].x<minx) minx=p[i].x;
        if(p[i].x>maxx) maxx=p[i].x;
        if(p[i].y<miny) miny=p[i].y;
        if(p[i].y>maxy) maxy=p[i].y;
    }
    float w=maxx-minx, h=maxy-miny, s = w>h?w:h; if(s<1) s=1;
    for(int i=0;i<KW_N;i++){ p[i].x=(p[i].x-minx)*KW_SQ/s; p[i].y=(p[i].y-miny)*KW_SQ/s; }
    Pt c; kw_centroid(p,&c);
    for(int i=0;i<KW_N;i++){ p[i].x-=c.x; p[i].y-=c.y; }
}
static void kw_rotate(Pt *p, float rad){
    Pt c; kw_centroid(p,&c); float co=cosf(rad), si=sinf(rad);
    for(int i=0;i<KW_N;i++){ float dx=p[i].x-c.x, dy=p[i].y-c.y;
        p[i].x=dx*co-dy*si+c.x; p[i].y=dx*si+dy*co+c.y; }
}
static float kw_dist_at(const Pt *a, Pt *b, float rad){
    Pt tmp[KW_N]; memcpy(tmp,b,sizeof tmp); kw_rotate(tmp,rad);
    float d=0; for(int i=0;i<KW_N;i++){ float dx=a[i].x-tmp[i].x, dy=a[i].y-tmp[i].y; d+=sqrtf(dx*dx+dy*dy); }
    return d/KW_N;
}
static float kw_dist_best(const Pt *a, Pt *b){
    float lo=-KW_ANGLE, hi=KW_ANGLE;
    float x1=KW_PHI*lo+(1-KW_PHI)*hi, f1=kw_dist_at(a,b,x1);
    float x2=(1-KW_PHI)*lo+KW_PHI*hi, f2=kw_dist_at(a,b,x2);
    for(int it=0; it<8; it++){
        if(f1<f2){ hi=x2; x2=x1; f2=f1; x1=KW_PHI*lo+(1-KW_PHI)*hi; f1=kw_dist_at(a,b,x1); }
        else     { lo=x1; x1=x2; f1=f2; x2=(1-KW_PHI)*lo+KW_PHI*hi; f2=kw_dist_at(a,b,x2); }
    }
    return f1<f2?f1:f2;
}

float kana_stroke_dist(const int16_t *uxy, int un, const KStroke *expect){
    if(!uxy || un<2 || !expect || expect->npts<1) return 1e9f;
    Pt ub[256]; if(un>256) un=256;
    for(int i=0;i<un;i++){ ub[i].x=(float)uxy[2*i]; ub[i].y=(float)uxy[2*i+1]; }
    Pt eb[256]; int en=expect->npts;
    for(int i=0;i<en;i++){ eb[i].x=(float)expect->pts[2*i]; eb[i].y=(float)expect->pts[2*i+1]; }
    /* a near-dot expected stroke (single point) can't be shape-matched -- accept any
     * short user stroke that is itself small (a tap/tick). */
    if(en<2){
        float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
        for(int i=0;i<un;i++){
            if(ub[i].x<minx) minx=ub[i].x;
            if(ub[i].x>maxx) maxx=ub[i].x;
            if(ub[i].y<miny) miny=ub[i].y;
            if(ub[i].y>maxy) maxy=ub[i].y;
        }
        float w=maxx-minx, h=maxy-miny;
        return (w<40 && h<40) ? 0.0f : 60.0f;
    }
    Pt ru[KW_N], re[KW_N];
    kw_resample(ub, un, ru); kw_normalize(ru);
    kw_resample(eb, en, re); kw_normalize(re);
    return kw_dist_best(ru, re);
}
