/* graf_test.c -- offline accuracy harness for the Graffiti $1 recognizer.
 *
 * For each template it synthesizes many hand-drawn-like strokes (the template
 * polyline densified + per-point Gaussian jitter, at a realistic pixel scale)
 * and runs them through the REAL graffiti_recognize(). It reports per-glyph
 * accuracy and the top confusions, and exits non-zero if accuracy falls below
 * the gate thresholds -- so a template edit that makes recognition worse fails
 * CI. Deterministic (fixed PRNG seed).
 *
 * Build: cc -DGRAF_TEST_HOOKS ... graffiti.c graf_test.c   (see sim/Makefile `graf`)
 */
#include "graffiti.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int graf_test_ntmpl(int which);
const float *graf_test_tmpl(int which, int i, char *ch, int *npairs);

/* ---- deterministic xorshift PRNG + approx-Gaussian ---- */
static unsigned long long g_rng = 88172645463325252ULL;
static double urand(void){
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (double)(g_rng >> 11) * (1.0 / 9007199254740992.0);
}
static double nrand(void){                 /* ~N(0,1): sum of 12 uniforms - 6 */
    double s = 0; for(int i=0;i<12;i++) s += urand(); return s - 6.0;
}

#define SCALE   9.0f      /* grid 0..10 -> ~0..90 px (bbox > TAP_PX/gesture cutoffs) */
#define STEPS   8         /* densify points per template segment */
#define MAXO    512

/* densify a template polyline (npairs control points, grid units) into integer
 * pixel points with per-point Gaussian jitter of sigma px. Returns point count. */
static int synth(const float *pts, int np, float sigma, int *ox, int *oy){
    int o = 0;
    for(int s=0; s<np-1 && o<MAXO-1; s++){
        float x0=pts[2*s], y0=pts[2*s+1], x1=pts[2*s+2], y1=pts[2*s+3];
        for(int k=0; k<STEPS && o<MAXO-1; k++){
            float t = (float)k / STEPS;
            float x = (x0 + (x1-x0)*t)*SCALE + (float)(sigma*nrand());
            float y = (y0 + (y1-y0)*t)*SCALE + (float)(sigma*nrand());
            ox[o]=(int)lroundf(x); oy[o]=(int)lroundf(y); o++;
        }
    }
    float x = pts[2*(np-1)]*SCALE   + (float)(sigma*nrand());
    float y = pts[2*(np-1)+1]*SCALE + (float)(sigma*nrand());
    ox[o]=(int)lroundf(x); oy[o]=(int)lroundf(y); o++;
    return o;
}

#define TRIALS  40

struct res { char ch; int correct; char worst_conf; int worst_n; };

/* arm the punctuation shift the way the UI does: a single tap (a sub-TAP_PX
 * stroke) makes graffiti_recognize return GRAF_PUNCT and read the NEXT stroke as
 * punctuation. */
static void arm_punct(void){
    graffiti_clear();
    graffiti_add_point(45,45); graffiti_add_point(46,45); graffiti_add_point(45,46);
    (void)graffiti_recognize(0);
}

static int run_set(int which, int digits, int punct, const char *label, float sigma,
                   float pass_each, float pass_mean){
    int nt = graf_test_ntmpl(which);
    struct res R[64];
    int total_correct = 0, total = 0, worst_glyph_fail = 0;

    for(int i=0;i<nt;i++){
        char ch; int np; const float *pts = graf_test_tmpl(which, i, &ch, &np);
        int conf[128]; memset(conf,0,sizeof conf);
        int correct = 0;
        for(int t=0;t<TRIALS;t++){
            int ox[MAXO], oy[MAXO];
            int n = synth(pts, np, sigma, ox, oy);
            if(punct) arm_punct();          /* two-step: tap arms, then the stroke */
            graffiti_clear();
            for(int j=0;j<n;j++) graffiti_add_point(ox[j], oy[j]);
            char c = graffiti_recognize(digits);
            if(c == ch) correct++;
            else conf[(unsigned char)(c?c:'?')]++;
        }
        char wc='?'; int wn=0;
        for(int k=0;k<128;k++) if(conf[k]>wn){ wn=conf[k]; wc=(char)k; }
        R[i].ch=ch; R[i].correct=correct; R[i].worst_conf=wc; R[i].worst_n=wn;
        total_correct += correct; total += TRIALS;
        if(correct < (int)(pass_each*TRIALS)) worst_glyph_fail++;
    }

    double mean = (double)total_correct / total;
    printf("\n== %s set  (sigma=%.1f px, %d trials/glyph) ==\n", label, sigma, TRIALS);
    printf("  mean accuracy: %.1f%%\n", 100.0*mean);
    /* list any glyph below the per-glyph bar, worst first */
    int printed = 0;
    for(int pass=0; pass<nt; pass++){
        int lo=1e9, li=-1;
        for(int i=0;i<nt;i++){ if(R[i].correct < lo){ lo=R[i].correct; li=i; } }
        if(li<0) break;
        if((double)R[li].correct/TRIALS < 0.999 || printed < 5){
            printf("    '%c': %3.0f%%  (%d/%d)%s%s\n", R[li].ch,
                   100.0*R[li].correct/TRIALS, R[li].correct, TRIALS,
                   R[li].worst_n? "  most-confused-> '":"",
                   R[li].worst_n? (char[]){R[li].worst_conf,'\'',0} : "");
            printed++;
        }
        R[li].correct = 1e9;   /* mark done */
    }

    int ok = (mean >= pass_mean) && (worst_glyph_fail == 0);
    printf("  -> %s (need mean>=%.0f%%, every glyph>=%.0f%%; %d below)\n",
           ok?"PASS":"FAIL", 100.0*pass_mean, 100.0*pass_each, worst_glyph_fail);
    return ok;
}

int main(void){
    /* the recognizer logs one ESP_LOGI line per stroke to stderr; silence the
     * ~1500 lines so CI output is just the accuracy summary on stdout. */
    if(!freopen("/dev/null", "w", stderr)) { /* non-fatal */ }
    printf("Graffiti recognizer accuracy harness\n");
    int ok = 1;
    /* letters + digits at a moderate jitter that approximates the resistive strip.
     * Thresholds are set to lock in the current templates; raise them as the
     * templates improve. */
    ok &= run_set(0, 0, 0, "letters", 3.0f, 0.80f, 0.95f);
    ok &= run_set(1, 1, 0, "digits",  3.0f, 0.80f, 0.95f);
    /* punctuation: a smaller, curvier set entered after the punct-shift; several
     * members are single straight lines that only differ by orientation, so the
     * bar is a touch lower than letters/digits. */
    ok &= run_set(2, 0, 1, "punct",   3.0f, 0.70f, 0.85f);
    printf("\n%s\n", ok ? "graf accuracy: OK" : "graf accuracy: FAIL");
    return ok ? 0 : 1;
}
