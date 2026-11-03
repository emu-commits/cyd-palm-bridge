/* crt.c -- see crt.h; the numbers are DEX_PLAN.md 2.4's. */
#include "crt.h"

#include <stdlib.h>

/* Q8: 256 is 1.0. The standard set is the one the owner chose from the mock-ups
 * (tools/mock_dex.py has the same sums); the other two are for choosing on the panel. */
typedef struct { uint16_t bloom, scan, dark; uint8_t lift[3]; } Set;
static const Set SETS[CRT_NSTRENGTHS] = {
    [CRT_OFF]      = {  0, 256,   0, { 0, 0, 0 } },
    [CRT_LIGHT]    = { 51, 230,  64, { 3, 4, 6 } },
    [CRT_STANDARD] = { 77, 205,  90, { 5, 6, 9 } },
    [CRT_STRONG]   = {102, 179, 115, { 7, 8, 11 } },
};
static const char *const NAMES[CRT_NSTRENGTHS] = { "Off", "Light", "Standard", "Strong" };

static struct {
    uint16_t *dx, *dy;          /* one block: (2 x d)^4 in Q8 across the box, then down it */
    int16_t x0, y0, x1, y1;
    uint16_t well;
    uint8_t radius, strength, on, paused;
    uint8_t inset[CRT_MAX_RADIUS];
} C = { .strength = CRT_STANDARD };

static uint16_t q8_pow4(int i, int n){
    /* d from -1 to 1 across n pixels: ((2i - (n - 1)) / n)^4 in Q8 */
    uint64_t t = (uint64_t)((2 * i - (n - 1)) * (2 * i - (n - 1)));
    uint64_t v = t * t * 256 / ((uint64_t)n * n * n * n);
    return (uint16_t)(v > 256 ? 256 : v);
}

static int isqrt(int v){
    int r = 0;
    while((r + 1) * (r + 1) <= v) r++;
    return r;
}

int crt_on(int x0, int y0, int x1, int y1, int radius, uint16_t well){
    crt_off();
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if(w < 2 || h < 2 || radius < 0 || radius > CRT_MAX_RADIUS) return -1;
    C.dx = malloc(((size_t)w + (size_t)h) * sizeof *C.dx);
    if(!C.dx) return -1;
    C.dy = C.dx + w;
    for(int i = 0; i < w; i++) C.dx[i] = q8_pow4(i, w);
    for(int i = 0; i < h; i++) C.dy[i] = q8_pow4(i, h);
    /* how far in each row near the top and bottom starts: a circle, tested at the
     * pixel's centre (2 x the radius, so the half-pixel stays whole) */
    for(int j = 0; j < radius; j++){
        int up = 2 * (radius - j) - 1;
        C.inset[j] = (uint8_t)(radius - (isqrt(4 * radius * radius - up * up) + 1) / 2);
    }
    C.x0 = (int16_t)x0; C.y0 = (int16_t)y0; C.x1 = (int16_t)x1; C.y1 = (int16_t)y1;
    C.radius = (uint8_t)radius;
    C.well = well;
    C.on = 1;
    return 0;
}

void crt_off(void){
    free(C.dx);
    C.dx = C.dy = NULL;
    C.on = C.paused = 0;
}

int crt_active(void){ return C.on; }
void crt_pause(int on){ C.paused = (uint8_t)!!on; }
int  crt_paused(void){ return C.paused; }

void crt_set_strength(int s){ if(s >= 0 && s < CRT_NSTRENGTHS) C.strength = (uint8_t)s; }
int  crt_strength(void){ return C.strength; }
const char *crt_strength_name(int s){ return s >= 0 && s < CRT_NSTRENGTHS ? NAMES[s] : "?"; }

void crt_widen(int *x1, int y1, int *x2, int y2){
    if(!C.on || y2 < C.y0 || y1 > C.y1 || *x2 < C.x0 || *x1 > C.x1) return;
    if(*x1 > C.x0) *x1 = C.x0;
    if(*x2 < C.x1) *x2 = C.x1;
}

#define R8(v) ((uint32_t)(((v) >> 8 & 0xF8) | ((v) >> 13)))
#define G8(v) ((uint32_t)(((v) >> 3 & 0xFC) | ((v) >> 9 & 3)))
#define B8(v) ((uint32_t)(((v) << 3 & 0xF8) | ((v) >> 2 & 7)))

void crt_flush(int x1, int y1, int x2, int y2, uint16_t *px){
    if(!C.on || C.paused || y2 < C.y0 || y1 > C.y1 || x2 < C.x0 || x1 > C.x1) return;
    const Set *s = &SETS[C.strength];
    const int ax = x1 > C.x0 ? x1 : C.x0, bx = x2 < C.x1 ? x2 : C.x1;
    const int ay = y1 > C.y0 ? y1 : C.y0, by = y2 < C.y1 ? y2 : C.y1;
    const int stride = x2 - x1 + 1, bloom = s->bloom, off = C.strength == CRT_OFF;
    const uint32_t lr = s->lift[0], lg = s->lift[1], lb = s->lift[2];
    for(int y = ay; y <= by; y++){
        uint16_t *row = px + (y - y1) * stride + (ax - x1);
        int rel = y - C.y0;
        if(!off){
            const uint32_t scan = rel & 1 ? s->scan : 256, dy = C.dy[rel], dark = s->dark;
            const uint16_t *dxs = C.dx + (ax - C.x0);
            uint16_t l = row[0], cur = row[0];
            /* the glow is the same as the last pixel's on a flat run, so it's kept */
            uint32_t mem_l = 0x10000, mem_c = 0, mem_r = 0, br = 0, bg = 0, bb = 0;
            for(int i = 0, n = bx - ax + 1; i < n; i++){
                uint16_t r = i + 1 < n ? row[i + 1] : cur;
                if(l != mem_l || cur != mem_c || r != mem_r){
                    mem_l = l; mem_c = cur; mem_r = r;
                    uint32_t cr = R8(cur), cg = G8(cur), cb = B8(cur);
                    /* v + bloom x (left + right) / 2 x (255 - v) / 255 */
                    br = cr + (((R8(l) + R8(r)) * (255 - cr) * (uint32_t)bloom) >> 17);
                    bg = cg + (((G8(l) + G8(r)) * (255 - cg) * (uint32_t)bloom) >> 17);
                    bb = cb + (((B8(l) + B8(r)) * (255 - cb) * (uint32_t)bloom) >> 17);
                }
                uint32_t d = dxs[i] + dy;
                if(d > 256) d = 256;
                uint32_t f = (scan * (256 - ((dark * d) >> 8))) >> 8;
                uint32_t rr = ((br * f) >> 8) + lr, gg = ((bg * f) >> 8) + lg, bo = ((bb * f) >> 8) + lb;
                if(rr > 255) rr = 255;
                if(gg > 255) gg = 255;
                if(bo > 255) bo = 255;
                row[i] = (uint16_t)((rr & 0xF8) << 8 | (gg & 0xFC) << 3 | bo >> 3);
                l = cur;
                cur = r;
            }
        }
        /* the corners, after the effect: they're the well, not the picture */
        int j = rel < C.radius ? rel : C.y1 - y < C.radius ? C.y1 - y : -1;
        if(j >= 0){
            int in = C.inset[j];
            for(int x = ax; x <= bx; x++)
                if(x - C.x0 < in || C.x1 - x < in) row[x - ax] = C.well;
        }
    }
}
