/* minesweeper.c -- see minesweeper.h. Pure C. */
#include "minesweeper.h"
#include <string.h>

/* small deterministic PRNG (xorshift32) so a seed reproduces a board exactly. */
static uint32_t xs(uint32_t *s){
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

void ms_new(MsGame *g, int w, int h, int mines, uint32_t seed){
    memset(g, 0, sizeof *g);
    if(w > MS_MAXW) w = MS_MAXW;
    if(h > MS_MAXH) h = MS_MAXH;
    if(mines > w*h - 9) mines = w*h - 9;    /* leave room for a safe 3x3 first tap */
    if(mines < 1) mines = 1;
    g->w = (uint8_t)w; g->h = (uint8_t)h; g->mines = (uint8_t)mines;
    g->state = MS_PLAY; g->first = 1; g->seed = seed;
}

/* is (r,c) within the safe zone around the first tap (rr,cc)? */
static int in_safe(int r, int c, int rr, int cc){
    int dr = r-rr, dc = c-cc;
    if(dr<0) dr=-dr;
    if(dc<0) dc=-dc;
    return dr<=1 && dc<=1;
}

static void place_mines(MsGame *g, int rr, int cc){
    int n = g->w * g->h, placed = 0;
    uint32_t s = g->seed;
    while(placed < g->mines){
        int idx = (int)(xs(&s) % (uint32_t)n);
        int r = idx / g->w, c = idx % g->w;
        if(g->cell[idx] & MS_MINE) continue;
        if(in_safe(r, c, rr, cc)) continue;
        g->cell[idx] |= MS_MINE; placed++;
    }
    /* adjacency counts */
    for(int r=0;r<g->h;r++) for(int c=0;c<g->w;c++){
        int cnt=0;
        for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++){
            if(!dr && !dc) continue;
            int nr=r+dr, nc=c+dc;
            if(nr<0||nc<0||nr>=g->h||nc>=g->w) continue;
            if(g->cell[nr*g->w+nc] & MS_MINE) cnt++;
        }
        g->adj[r*g->w+c] = (int8_t)cnt;
    }
    g->first = 0;
}

/* flood-reveal from (r,c): reveal it; if zero-adjacency, spread to neighbours.
 * Cells are marked revealed WHEN PUSHED (not when popped), so each is pushed at
 * most once and the stack never exceeds the cell count. */
static void flood(MsGame *g, int r0, int c0){
    int stack[MS_MAXW*MS_MAXH], sp=0;
    int i0 = r0*g->w + c0;
    if(g->cell[i0] & (MS_REVEALED|MS_FLAG)) return;
    g->cell[i0] |= MS_REVEALED;
    stack[sp++] = i0;
    while(sp){
        int idx = stack[--sp];
        if(g->adj[idx] != 0) continue;               /* border of the empty region */
        int r = idx/g->w, c = idx%g->w;
        for(int dr=-1;dr<=1;dr++) for(int dc=-1;dc<=1;dc++){
            if(!dr && !dc) continue;
            int nr=r+dr, nc=c+dc;
            if(nr<0||nc<0||nr>=g->h||nc>=g->w) continue;
            int ni = nr*g->w+nc;
            if(!(g->cell[ni] & (MS_REVEALED|MS_FLAG))){
                g->cell[ni] |= MS_REVEALED;           /* mark on push -> pushed once */
                stack[sp++] = ni;
            }
        }
    }
}

static int check_win(MsGame *g){
    for(int i=0;i<g->w*g->h;i++)
        if(!(g->cell[i]&MS_MINE) && !(g->cell[i]&MS_REVEALED)) return 0;
    return 1;                                          /* every non-mine revealed */
}

int ms_reveal(MsGame *g, int r, int c){
    if(g->state != MS_PLAY) return g->state;
    if(r<0||c<0||r>=g->h||c>=g->w) return g->state;
    int idx = r*g->w+c;
    if(g->cell[idx] & (MS_REVEALED|MS_FLAG)) return g->state;
    if(g->first) place_mines(g, r, c);
    if(g->cell[idx] & MS_MINE){
        g->cell[idx] |= MS_REVEALED;
        g->state = MS_LOST;
        return g->state;
    }
    flood(g, r, c);
    if(check_win(g)) g->state = MS_WON;
    return g->state;
}

void ms_flag(MsGame *g, int r, int c){
    if(g->state != MS_PLAY) return;
    if(r<0||c<0||r>=g->h||c>=g->w) return;
    int idx = r*g->w+c;
    if(g->cell[idx] & MS_REVEALED) return;
    g->cell[idx] ^= MS_FLAG;
}

int ms_flags(const MsGame *g){
    int n=0; for(int i=0;i<g->w*g->h;i++) if(g->cell[i]&MS_FLAG) n++;
    return n;
}
