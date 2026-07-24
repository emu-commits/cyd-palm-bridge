/* zip.c -- see zip.h. Pure C: a seeded generator (random Hamiltonian path ->
 * ordered waypoints -> uniqueness check) plus the path-drawing rules. No LVGL, no
 * allocation.
 *
 * RAM notes (no PSRAM on this board, and LVGL runs on a 12 KB-stack task):
 *  - every search scratch buffer is file-scope static, NOT a recursive local, and
 *    neighbours come from a shared table, so the 36-deep DFS frames stay small.
 *    Measured with -fstack-usage: 272 (zp_new) + 36*80 (cs_dfs) + 40 (reach_ok)
 *    = ~3.2 KB worst case, which is the whole reason for the table;
 *  - the flood-fill connectivity prune is iterative over a 36-byte stack;
 *  - the searches carry a node BUDGET so a pathological board can never spin the
 *    watchdog: running out just means "not proven unique", and the generator keeps
 *    that number instead of removing it.
 *  - total static footprint is ~330 bytes; a ZpGame is 116.
 */
#include "zip.h"
#include <string.h>

/* xorshift32 (the same PRNG family as the other games) for deterministic seeds. */
static uint32_t xs(uint32_t *s){
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}

/* Orthogonal-neighbour table, built once. This is a stack optimisation as much as
 * a speed one: the searches recurse 36 deep on a device whose UI task has a 12 KB
 * stack, and reading a shared table instead of filling a local `int8_t nb[4]` in
 * every frame cut the measured worst-case depth by a third (-fstack-usage). */
static uint8_t NB[ZP_CELLS][4], NBN[ZP_CELLS], nb_ready;
static void nb_init(void){
    if(nb_ready) return;
    for(int p = 0; p < ZP_CELLS; p++){
        int r = p / ZP_N, c = p % ZP_N, n = 0;
        if(r > 0)        NB[p][n++] = (uint8_t)(p - ZP_N);
        if(r < ZP_N - 1) NB[p][n++] = (uint8_t)(p + ZP_N);
        if(c > 0)        NB[p][n++] = (uint8_t)(p - 1);
        if(c < ZP_N - 1) NB[p][n++] = (uint8_t)(p + 1);
        NBN[p] = (uint8_t)n;
    }
    nb_ready = 1;
}

int zp_adjacent(int a, int b){
    if(a < 0 || b < 0 || a >= ZP_CELLS || b >= ZP_CELLS) return 0;
    int ra = a / ZP_N, ca = a % ZP_N, rb = b / ZP_N, cb = b % ZP_N;
    int dr = ra > rb ? ra - rb : rb - ra;
    int dc = ca > cb ? ca - cb : cb - ca;
    return (dr + dc) == 1;
}

/* ---- the shared prune: can the rest of the board still be covered? -------------
 * From `cur`, every unvisited cell must be reachable through unvisited cells --
 * otherwise the path has already orphaned part of the grid and no completion
 * exists. This one check does most of the work of making a Hamiltonian search on
 * 36 cells feel instant. Scratch is static (see the file header). */
static uint8_t rk_seen[ZP_CELLS], rk_stk[ZP_CELLS];
static int reach_ok(const uint8_t *vis, int cur, int nunvis){
    if(nunvis == 0) return 1;
    int seed = -1;
    for(int i = 0; i < NBN[cur]; i++) if(!vis[NB[cur][i]]){ seed = NB[cur][i]; break; }
    if(seed < 0) return 0;                       /* cells left, nowhere to step */
    memset(rk_seen, 0, sizeof rk_seen);
    int sp = 0, cnt = 0;
    rk_stk[sp++] = (uint8_t)seed; rk_seen[seed] = 1;
    while(sp){
        int p = rk_stk[--sp]; cnt++;
        for(int i = 0; i < NBN[p]; i++){
            int t = NB[p][i];
            if(!vis[t] && !rk_seen[t]){ rk_seen[t] = 1; rk_stk[sp++] = (uint8_t)t; }
        }
    }
    return cnt == nunvis;
}

/* ---- random Hamiltonian path (the reference solution) ------------------------- */
static uint8_t  hm_vis[ZP_CELLS], hm_path[ZP_CELLS];
static uint32_t hm_budget;

static int hm_dfs(int cur, int depth, uint32_t *rng){
    if(depth == ZP_CELLS) return 1;
    if(hm_budget == 0) return 0;
    hm_budget--;
    if(!reach_ok(hm_vis, cur, ZP_CELLS - depth)) return 0;
    uint8_t nb[4]; int n = NBN[cur];
    for(int i = 0; i < n; i++) nb[i] = NB[cur][i];
    for(int i = n - 1; i > 0; i--){              /* shuffle the step order */
        int j = (int)(xs(rng) % (uint32_t)(i + 1));
        uint8_t t = nb[i]; nb[i] = nb[j]; nb[j] = t;
    }
    for(int i = 0; i < n; i++){
        int q = nb[i];
        if(hm_vis[q]) continue;
        hm_vis[q] = 1; hm_path[depth] = (uint8_t)q;
        if(hm_dfs(q, depth + 1, rng)) return 1;
        hm_vis[q] = 0;
        if(hm_budget == 0) return 0;
    }
    return 0;
}
/* fill hm_path[] with a Hamiltonian path over the whole grid. 1 on success. */
static int ham_path(uint32_t *rng){
    nb_init();
    for(int attempt = 0; attempt < 24; attempt++){
        memset(hm_vis, 0, sizeof hm_vis);
        int s = (int)(xs(rng) % ZP_CELLS);
        hm_vis[s] = 1; hm_path[0] = (uint8_t)s;
        hm_budget = 200000u;
        if(hm_dfs(s, 1, rng)) return 1;
    }
    return 0;
}

/* ---- solution counter (uniqueness) ------------------------------------------- */
static const uint8_t *cs_num;
static uint8_t  cs_vis[ZP_CELLS];
static int      cs_limit, cs_found;
static uint32_t cs_budget;

static void cs_dfs(int cur, int depth, int nextn){
    if(cs_found >= cs_limit || cs_budget == 0) return;
    cs_budget--;
    if(depth == ZP_CELLS){ cs_found++; return; }
    if(!reach_ok(cs_vis, cur, ZP_CELLS - depth)) return;
    for(int i = 0; i < NBN[cur]; i++){
        int q = NB[cur][i];
        if(cs_vis[q]) continue;
        int w = cs_num[q];
        if(w && w != nextn) continue;            /* a number out of turn */
        cs_vis[q] = 1;
        cs_dfs(q, depth + 1, w ? nextn + 1 : nextn);
        cs_vis[q] = 0;
        if(cs_found >= cs_limit || cs_budget == 0) return;
    }
}
/* count completions of `num`, capped at `limit`. `spent_all` (optional) comes back
 * 1 if the node budget ran out, i.e. the count is a floor, not a proof. */
static int count_paths(const uint8_t *num, int limit, uint32_t budget, int *spent_all){
    nb_init();
    int start = -1;
    for(int p = 0; p < ZP_CELLS; p++) if(num[p] == 1){ start = p; break; }
    if(spent_all) *spent_all = 0;
    if(start < 0) return 0;
    cs_num = num; cs_limit = limit; cs_found = 0; cs_budget = budget;
    memset(cs_vis, 0, sizeof cs_vis);
    cs_vis[start] = 1;
    cs_dfs(start, 1, 2);                         /* 1 is placed; 2 is next */
    if(spent_all && cs_budget == 0 && cs_found < cs_limit) *spent_all = 1;
    return cs_found;
}

int zp_count_solutions(const ZpGame *g, int limit){
    return count_paths(g->num, limit, 20000000u, NULL);
}

/* ---- generation ---------------------------------------------------------------
 * Number the reference path's kept positions 1..n in path order. Dropping a
 * position never reorders the rest, so the puzzle's meaning is unchanged -- the
 * numbers still have to be hit in the same sequence. */
static void lay_numbers(uint8_t *num, const uint8_t *keep){
    memset(num, 0, ZP_CELLS);
    int n = 0;
    for(int i = 0; i < ZP_CELLS; i++) if(keep[i]) num[hm_path[i]] = (uint8_t)(++n);
}

/* Spreading a fixed number of waypoints evenly does NOT pin a path down: measured
 * over 6x6 boards, 10 evenly spaced numbers still left 2-41 solutions. So generate
 * the way sudoku.c does -- start from a board where EVERY cell is numbered (the
 * numbers spell out the path, so it is trivially unique) and remove numbers one at
 * a time, keeping a removal only while exactly one solution survives. What's left
 * is a minimal fair puzzle: no number in it is redundant.
 *
 * This is affordable because the connectivity prune makes an exhaustive count cost
 * only a few thousand nodes on 36 cells -- the whole New game is ~35 of those. */
void zp_new(ZpGame *g, uint32_t seed){
    memset(g, 0, sizeof *g);
    g->seed = seed;
    uint32_t rng = seed ? seed : 1u;

    if(!ham_path(&rng)){                         /* can't happen on an even grid, but
                                                  * degrade to a legal board, not a hang */
        for(int p = 0; p < ZP_CELLS; p++){
            int r = p / ZP_N, c = p % ZP_N;      /* boustrophedon: a valid Ham path */
            hm_path[p] = (uint8_t)(r * ZP_N + ((r & 1) ? (ZP_N - 1 - c) : c));
        }
    }

    uint8_t keep[ZP_CELLS];
    memset(keep, 1, sizeof keep);
    int order[ZP_CELLS];
    for(int i = 0; i < ZP_CELLS; i++) order[i] = i;
    for(int i = ZP_CELLS - 1; i > 0; i--){       /* random removal order, seeded */
        int j = (int)(xs(&rng) % (uint32_t)(i + 1));
        int t = order[i]; order[i] = order[j]; order[j] = t;
    }
    int kept = ZP_CELLS;
    for(int k = 0; k < ZP_CELLS; k++){
        int i = order[k];
        /* the ends stay: number 1 is where the pen must start, and the last number
         * gives the puzzle a visible finish line. */
        if(i == 0 || i == ZP_CELLS - 1) continue;
        keep[i] = 0;
        lay_numbers(g->num, keep);
        int blown = 0;
        if(count_paths(g->num, 2, 2000000u, &blown) == 1 && !blown) kept--;
        else keep[i] = 1;                        /* it would branch -> put it back */
    }
    lay_numbers(g->num, keep);
    g->nnum = (uint8_t)kept;

    zp_clear(g);
}

/* ---- play --------------------------------------------------------------------- */
int zp_start_cell(const ZpGame *g){
    for(int p = 0; p < ZP_CELLS; p++) if(g->num[p] == 1) return p;
    return -1;
}
int zp_head(const ZpGame *g){
    return g->plen ? (int)g->path[g->plen - 1] : -1;
}
int zp_remaining(const ZpGame *g){
    return ZP_CELLS - (int)g->plen;
}
int zp_solved(const ZpGame *g){
    return g->plen == ZP_CELLS;
}
static void zp_update_state(ZpGame *g){
    g->state = zp_solved(g) ? ZP_SOLVED : ZP_PLAY;
}
void zp_clear(ZpGame *g){
    memset(g->on, 0, sizeof g->on);
    g->plen = 0;
    int s = zp_start_cell(g);
    if(s >= 0){ g->path[0] = (uint8_t)s; g->on[s] = 1; g->plen = 1; }
    zp_update_state(g);
}
int zp_next_num(const ZpGame *g){
    int mx = 0;
    for(int i = 0; i < g->plen; i++){ int w = g->num[g->path[i]]; if(w > mx) mx = w; }
    return mx + 1;
}
int zp_can_extend(const ZpGame *g, int cell){
    if(cell < 0 || cell >= ZP_CELLS) return 0;
    if(g->on[cell]) return 0;
    int h = zp_head(g);
    if(h < 0 || !zp_adjacent(h, cell)) return 0;
    int w = g->num[cell];
    if(w && w != zp_next_num(g)) return 0;       /* numbers only in order */
    return 1;
}
int zp_extend(ZpGame *g, int cell){
    if(!zp_can_extend(g, cell)) return 0;
    g->path[g->plen++] = (uint8_t)cell;
    g->on[cell] = 1;
    zp_update_state(g);
    return 1;
}
int zp_truncate(ZpGame *g, int cell){
    if(cell < 0 || cell >= ZP_CELLS || !g->on[cell]) return 0;
    int idx = -1;
    for(int i = 0; i < g->plen; i++) if(g->path[i] == cell){ idx = i; break; }
    if(idx < 0 || idx == g->plen - 1) return 0;
    for(int i = idx + 1; i < g->plen; i++) g->on[g->path[i]] = 0;
    g->plen = (uint8_t)(idx + 1);
    zp_update_state(g);
    return 1;
}
int zp_undo(ZpGame *g){
    if(g->plen <= 1) return 0;
    return zp_truncate(g, g->path[g->plen - 2]);
}
int zp_touch(ZpGame *g, int cell){
    if(cell < 0 || cell >= ZP_CELLS) return 0;
    nb_init();
    int h = zp_head(g);
    if(h < 0 || cell == h) return 0;
    if(g->on[cell]) return zp_truncate(g, cell);
    if(zp_extend(g, cell)) return 1;
    /* A drag that outran the sampler (or cut a diagonal) lands two steps away.
     * Bridge through a shared free neighbour when every rule still holds -- this is
     * what makes drawing with a fingertip on a resistive panel feel right. */
    for(int i = 0; i < NBN[cell]; i++){
        int mid = NB[cell][i];
        if(!zp_extend(g, mid)) continue;
        if(zp_extend(g, cell)) return 1;
        zp_undo(g);                              /* that bridge was a dead end */
    }
    return 0;
}
