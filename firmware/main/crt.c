/* crt.c -- see crt.h and DEX_PLAN.md 2.4.
 *
 * MEASURED on the board (a walking frame of Critters, 232 x 172, BUILD_PROGRESS.md):
 * the first version, a sum for each channel of each pixel, took 38 ms of a 94 ms frame
 * at 160 MHz. The full effect here does the three channels at once in one 32-bit word.
 * The glow's "(255 - v)" is the pixel's green rather than each channel's own, a small
 * error on saturated colours, which is what makes one multiply do for all three. That
 * is about 100 cycles a pixel, still 17 ms a frame at 240 MHz: most of it is loads
 * (7 cycles each, here). Moving the loop to IRAM made no difference: it's the data,
 * not the code. That's why the effect is for screens that don't animate. */
#include "crt.h"

#include <stdlib.h>

/* scan: odd rows, of 256. side: how much a side's edge loses, of 256, so a corner
 * loses twice that (very little, and gradually: the owner, from the bench). glow: of
 * 32. lift: added to every pixel, in the channels' own steps. */
typedef struct { uint16_t scan; uint8_t side, glow; uint8_t lift_r, lift_g, lift_b; } Set;
static const Set SETS[CRT_NSTRENGTHS] = {
    [CRT_OFF]      = { 256,  0,  0, 0, 0, 0 },
    [CRT_LIGHT]    = { 230, 10,  6, 0, 1, 0 },
    [CRT_STANDARD] = { 205, 15, 10, 0, 1, 1 },
    [CRT_STRONG]   = { 179, 22, 13, 0, 2, 1 },
};
static const char *const NAMES[CRT_NSTRENGTHS] = { "Off", "Light", "Standard", "Strong" };

static struct {
    uint8_t *dx, *dy;           /* one block: what each column and row loses, of 256, at this strength */
    uint16_t *corner;           /* ...and the corners' colours: radius x radius, the top left's */
    int16_t x0, y0, x1, y1;
    uint8_t radius, strength, on, plain;
    int16_t ex1, ey1, ex2, ey2;  /* left alone: ex2 < ex1 for nothing */
    uint8_t inset[CRT_MAX_RADIUS];
} C = { .strength = CRT_STANDARD };

/* the fade: d^2 from the middle (0) to the edge (1), times `side`. Not d^4: that's
 * flat until most of the way out and then falls away quickly. */
static void tables(void){
    if(!C.on) return;
    int w = C.x1 - C.x0 + 1, h = C.y1 - C.y0 + 1, side = SETS[C.strength].side;
    for(int k = 0; k < 2; k++){
        int n = k ? h : w;
        uint8_t *t = k ? C.dy : C.dx;
        for(int i = 0; i < n; i++){
            int d = 2 * i - (n - 1);
            t[i] = (uint8_t)((int64_t)d * d * side / ((int64_t)n * n));
        }
    }
}

static uint16_t mix565(uint16_t a, uint16_t b, int of16){
    int r = ((a >> 11) * (16 - of16) + (b >> 11) * of16 + 8) / 16;
    int g = (((a >> 5) & 63) * (16 - of16) + ((b >> 5) & 63) * of16 + 8) / 16;
    int l = ((a & 31) * (16 - of16) + (b & 31) * of16 + 8) / 16;
    return (uint16_t)(r << 11 | g << 5 | l);
}

/* The corners, in a 4 x 4 grid to each pixel: the picture inside `radius`, then a
 * ring of the well one pixel wider, then whatever's round the box, softened as the
 * rounded well LVGL draws beside it is. */
static void corners(int radius, uint16_t well, uint16_t outside){
    const int r8 = 8 * radius, in = r8 * r8, out = (r8 + 8) * (r8 + 8);
    for(int j = 0; j < radius; j++){
        C.inset[j] = 0;
        for(int i = 0; i < radius; i++){
            int tube = 0, ring = 0;
            for(int v = 0; v < 4; v++)
                for(int u = 0; u < 4; u++){
                    int dx = r8 - (8 * i + 2 * u + 1), dy = r8 - (8 * j + 2 * v + 1), d = dx * dx + dy * dy;
                    tube += d <= in;
                    ring += d <= out;
                }
            C.corner[j * radius + i] = mix565(outside, well, ring);
            if(tube < 8) C.inset[j] = (uint8_t)(i + 1);
        }
    }
}

int crt_on(int x0, int y0, int x1, int y1, int radius, uint16_t well, uint16_t outside){
    crt_off();
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if(w < 2 || h < 2 || radius < 0 || radius > CRT_MAX_RADIUS) return -1;
    C.dx = malloc((size_t)w + (size_t)h + (size_t)radius * radius * sizeof *C.corner + sizeof *C.corner);
    if(!C.dx) return -1;
    /* the corners first in the block: they're the ones that want aligning */
    C.corner = (uint16_t *)C.dx;
    C.dx = (uint8_t *)(C.corner + radius * radius);
    C.dy = C.dx + w;
    corners(radius, well, outside);
    C.x0 = (int16_t)x0; C.y0 = (int16_t)y0; C.x1 = (int16_t)x1; C.y1 = (int16_t)y1;
    C.radius = (uint8_t)radius;
    C.ex1 = C.ey1 = 0; C.ex2 = C.ey2 = -1;
    C.on = 1;
    tables();
    return 0;
}

void crt_off(void){
    free(C.corner);
    C.corner = NULL;
    C.dx = C.dy = NULL;
    C.on = 0;
    C.plain = 0;
    C.ex1 = C.ey1 = 0; C.ex2 = C.ey2 = -1;
}

void crt_plain(int on){ C.plain = (uint8_t)!!on; }

int crt_active(void){ return C.on; }
int crt_exclude(int x1, int y1, int x2, int y2){
    if(x2 < x1) x1 = y1 = 0, x2 = y2 = -1;
    if(C.ex1 == x1 && C.ey1 == y1 && C.ex2 == x2 && C.ey2 == y2) return 0;
    C.ex1 = (int16_t)x1; C.ey1 = (int16_t)y1; C.ex2 = (int16_t)x2; C.ey2 = (int16_t)y2;
    return 1;
}

void crt_set_strength(int s){
    if(s < 0 || s >= CRT_NSTRENGTHS) return;
    C.strength = (uint8_t)s;
    tables();
}
int  crt_strength(void){ return C.strength; }
const char *crt_strength_name(int s){ return s >= 0 && s < CRT_NSTRENGTHS ? NAMES[s] : "?"; }

void crt_widen(int *x1, int y1, int *x2, int y2){
    if(!C.on || C.plain || y2 < C.y0 || y1 > C.y1 || *x2 < C.x0 || *x1 > C.x1) return;
    if(*x1 > C.x0) *x1 = C.x0;
    if(*x2 < C.x1) *x2 = C.x1;
}

/* RGB565 spread over a word so each channel has room above it: green at 21, red at 11 */
#define SPREAD_MASK 0x07E0F81Fu
#define SPREAD(c)   (((uint32_t)(c) | (uint32_t)(c) << 16) & SPREAD_MASK)
#define OVER_MASK   ((1u << 27) | (1u << 16) | (1u << 5))

/* The fade's steps would show as bands on a flat colour, the panel having only 32
 * levels of red and blue: rounding by turns in a checkerboard hides them, and at full
 * strength it rounds nothing, so the middle of the picture is left exact. */
#define ROUND_RB(p) ((p) ? 48u | 48u << 11 : 16u | 16u << 11)
#define ROUND_G(p)  ((p) ? 48u << 5 : 16u << 5)
#define FLIP_RB     ((16u ^ 48u) | (16u ^ 48u) << 11)
#define FLIP_G      ((16u ^ 48u) << 5)

/* row[a..b] of a row n long, in place, the whole effect. A pixel's neighbours are read
 * before they're written (l is kept), and past either end of the row it's its own
 * neighbour. */
static void row_effect(uint16_t *row, int a, int b, int n, const uint8_t *dxs, uint32_t base,
                       uint32_t glow, uint32_t lift, int parity){
    uint32_t l = SPREAD(a > 0 ? row[a - 1] : row[a]), c = SPREAD(row[a]);
    uint32_t rb_round = ROUND_RB(parity), g_round = ROUND_G(parity);
    for(int i = a; i <= b; i++){
        const uint32_t r = i + 1 < n ? SPREAD(row[i + 1]) : c;
        uint32_t add = ((((l + r) >> 1) & SPREAD_MASK) * glow >> 5) & SPREAD_MASK;
        add = (add * ((63 - (c >> 21)) >> 1) >> 5) & SPREAD_MASK;
        uint32_t x = c + add + lift, over = x & OVER_MASK;
        x = (x | (over - (over >> 5))) & SPREAD_MASK;
        const uint32_t lit = (x | x >> 16) & 0xFFFF, f = (base - dxs[i]) >> 2;     /* f: of 64 */
        row[i] = (uint16_t)((((lit & 0xF81F) * f + rb_round) >> 6 & 0xF81F) | (((lit & 0x07E0) * f + g_round) >> 6 & 0x07E0));
        rb_round ^= FLIP_RB;
        g_round ^= FLIP_G;
        l = c;
        c = r;
    }
}

void crt_flush(int x1, int y1, int x2, int y2, uint16_t *px){
    if(!C.on || y2 < C.y0 || y1 > C.y1 || x2 < C.x0 || x1 > C.x1) return;
    const Set *s = &SETS[C.strength];
    const int ax = x1 > C.x0 ? x1 : C.x0, bx = x2 < C.x1 ? x2 : C.x1;
    const int ay = y1 > C.y0 ? y1 : C.y0, by = y2 < C.y1 ? y2 : C.y1;
    const int stride = x2 - x1 + 1, off = C.plain || C.strength == CRT_OFF, n = bx - ax + 1;
    const uint32_t lift = (uint32_t)s->lift_g << 21 | (uint32_t)s->lift_r << 11 | s->lift_b;
    const uint8_t *dxs = C.dx + (ax - C.x0);
    for(int y = ay; y <= by; y++){
        uint16_t *row = px + (y - y1) * stride + (ax - x1);
        const int rel = y - C.y0;
        /* this row's part left alone, as positions in row[]: mb < ma for none */
        const int in_ex = y >= C.ey1 && y <= C.ey2;
        int ma = in_ex ? C.ex1 - ax : 0, mb = in_ex ? C.ex2 - ax : -1;
        if(ma < 0) ma = 0;
        if(mb >= n) mb = n - 1;
        if(!off){
            const uint32_t scan = rel & 1 ? s->scan : 256, base = scan * (256 - C.dy[rel]) >> 8;
            /* up to two parts, round what's left alone (whose pixels are neighbours, unwritten) */
            const int a1 = mb < ma ? n - 1 : ma - 1, a2 = mb < ma ? n : mb + 1;
            if(a1 >= 0) row_effect(row, 0, a1, n, dxs, base, s->glow, lift, (rel ^ ax) & 1);
            if(a2 < n) row_effect(row, a2, n - 1, n, dxs, base, s->glow, lift, (rel ^ ax ^ a2) & 1);
        }
        /* the corners, after the effect: they're the well, not the picture */
        const int j = rel < C.radius ? rel : C.y1 - y < C.radius ? C.y1 - y : -1;
        if(j >= 0){
            const int in = C.inset[j];
            const uint16_t *col = C.corner + j * C.radius;
            for(int x = ax; x <= bx; x++){
                const int left = x - C.x0, right = C.x1 - x, at = x - ax;
                if((left < in || right < in) && !(at >= ma && at <= mb)) row[at] = col[left < in ? left : right];
            }
        }
    }
}
