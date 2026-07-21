/* sudoku.c -- see sudoku.h. Pure C: a seeded generator that guarantees a unique
 * solution, plus placement / conflict / solved queries. No LVGL, no allocation. */
#include "sudoku.h"
#include <string.h>

/* xorshift32 PRNG (same family the other games use) for deterministic seeds. */
static uint32_t xs(uint32_t *s){
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

/* can `val` (1..9) go at index p (row-major) on `b` without breaking a rule? */
static int can_place(const uint8_t *b, int p, int val){
    int r = p / SD_N, c = p % SD_N;
    int br = (r/3)*3, bc = (c/3)*3;
    for(int i=0;i<SD_N;i++){
        if(b[r*SD_N + i] == val) return 0;      /* row */
        if(b[i*SD_N + c] == val) return 0;      /* column */
    }
    for(int dr=0;dr<3;dr++) for(int dc=0;dc<3;dc++)
        if(b[(br+dr)*SD_N + (bc+dc)] == val) return 0;   /* box */
    return 1;
}

/* fill an empty board with a random full valid solution (backtracking with a
 * shuffled candidate order). Returns 1 on success. */
static int fill_solution(uint8_t *b, int p, uint32_t *rng){
    while(p < SD_CELLS && b[p]) p++;
    if(p >= SD_CELLS) return 1;
    uint8_t order[SD_N] = {1,2,3,4,5,6,7,8,9};
    for(int i=SD_N-1;i>0;i--){ int j = (int)(xs(rng) % (uint32_t)(i+1)); uint8_t t=order[i]; order[i]=order[j]; order[j]=t; }
    for(int i=0;i<SD_N;i++){
        if(can_place(b, p, order[i])){
            b[p] = order[i];
            if(fill_solution(b, p+1, rng)) return 1;
            b[p] = 0;
        }
    }
    return 0;
}

/* count solutions of `b`, stopping once `limit` is reached (so uniqueness is a
 * cheap `count == 1` check). */
static void count_solutions(uint8_t *b, int p, int limit, int *count){
    if(*count >= limit) return;
    while(p < SD_CELLS && b[p]) p++;
    if(p >= SD_CELLS){ (*count)++; return; }
    for(int v=1; v<=SD_N; v++){
        if(can_place(b, p, v)){
            b[p] = v;
            count_solutions(b, p+1, limit, count);
            b[p] = 0;
            if(*count >= limit) return;
        }
    }
}

void sd_new(SdGame *g, uint32_t seed, int holes){
    memset(g, 0, sizeof *g);
    g->seed = seed;
    uint32_t rng = seed ? seed : 1u;

    uint8_t sol[SD_CELLS] = {0};
    fill_solution(sol, 0, &rng);
    memcpy(g->given, sol, SD_CELLS);
    memcpy(g->cell,  sol, SD_CELLS);

    /* dig holes in a random order, keeping the solution unique. */
    int order[SD_CELLS];
    for(int i=0;i<SD_CELLS;i++) order[i]=i;
    for(int i=SD_CELLS-1;i>0;i--){ int j=(int)(xs(&rng)%(uint32_t)(i+1)); int t=order[i]; order[i]=order[j]; order[j]=t; }

    if(holes < 0) holes = 0;
    if(holes > 64) holes = 64;                  /* keep at least 17 clues */
    int removed = 0;
    for(int k=0;k<SD_CELLS && removed<holes;k++){
        int p = order[k];
        uint8_t save = g->given[p];
        if(!save) continue;
        g->given[p] = 0; g->cell[p] = 0;
        uint8_t tmp[SD_CELLS]; memcpy(tmp, g->given, SD_CELLS);
        int cnt = 0; count_solutions(tmp, 0, 2, &cnt);
        if(cnt == 1){ removed++; }               /* still unique -> keep the hole */
        else { g->given[p] = save; g->cell[p] = save; }   /* would branch -> restore */
    }
    g->state = SD_PLAY;
}

int sd_is_given(const SdGame *g, int r, int c){
    if(r<0||r>=SD_N||c<0||c>=SD_N) return 0;
    return g->given[r*SD_N + c] != 0;
}

/* recompute the solved flag from the board. */
static void sd_update_state(SdGame *g){
    g->state = sd_solved(g) ? SD_SOLVED : SD_PLAY;
}

int sd_set(SdGame *g, int r, int c, int val){
    if(r<0||r>=SD_N||c<0||c>=SD_N) return 0;
    if(val<0||val>SD_N) return 0;
    int p = r*SD_N + c;
    if(g->given[p]) return 0;                    /* clue cells are fixed */
    if(g->cell[p] == (uint8_t)val) return 0;
    g->cell[p] = (uint8_t)val;
    sd_update_state(g);
    return 1;
}

int sd_conflict(const SdGame *g, int r, int c){
    if(r<0||r>=SD_N||c<0||c>=SD_N) return 0;
    int p = r*SD_N + c; uint8_t v = g->cell[p];
    if(!v) return 0;
    int br=(r/3)*3, bc=(c/3)*3;
    for(int i=0;i<SD_N;i++){
        if(i!=c && g->cell[r*SD_N + i] == v) return 1;
        if(i!=r && g->cell[i*SD_N + c] == v) return 1;
    }
    for(int dr=0;dr<3;dr++) for(int dc=0;dc<3;dc++){
        int rr=br+dr, cc=bc+dc;
        if((rr!=r||cc!=c) && g->cell[rr*SD_N + cc] == v) return 1;
    }
    return 0;
}

int sd_remaining(const SdGame *g){
    int n=0; for(int i=0;i<SD_CELLS;i++) if(!g->cell[i]) n++;
    return n;
}

int sd_solved(const SdGame *g){
    for(int i=0;i<SD_CELLS;i++) if(!g->cell[i]) return 0;   /* must be full */
    for(int r=0;r<SD_N;r++) for(int c=0;c<SD_N;c++) if(sd_conflict(g,r,c)) return 0;
    return 1;
}
