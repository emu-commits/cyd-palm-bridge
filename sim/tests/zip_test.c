/* zip_test.c -- host gate for the pure Zip path-puzzle logic (no LVGL).
 *
 * Proves the three things the UI trusts: every generated board is SOLVABLE (and
 * aims for a single solution), the drawing rules can't be cheated (no diagonals,
 * no revisits, no numbers out of turn), and the generator is deterministic and
 * fast enough to run on a 240 MHz MCU inside a button press. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "zip.h"

static int fails = 0;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } else printf("  ok: %s\n",(m)); }while(0)

/* An independent solver, written against zip.h only, so the test does not lean on
 * the module's internals: plain DFS with no pruning at all, capped by a node
 * budget. It walks the grid itself and applies the rules from the header. */
static int ref_num[ZP_CELLS];
static int ref_vis[ZP_CELLS];
static long ref_nodes;
static int ref_dfs(int cur, int depth, int nextn, int limit, int *found){
    if(*found >= limit) return 1;
    if(++ref_nodes > 40000000L) return 0;
    if(depth == ZP_CELLS){ (*found)++; return *found >= limit; }
    int r = cur / ZP_N, c = cur % ZP_N;
    int cand[4], n = 0;
    if(r > 0)        cand[n++] = cur - ZP_N;
    if(r < ZP_N - 1) cand[n++] = cur + ZP_N;
    if(c > 0)        cand[n++] = cur - 1;
    if(c < ZP_N - 1) cand[n++] = cur + 1;
    for(int i = 0; i < n; i++){
        int q = cand[i];
        if(ref_vis[q]) continue;
        int w = ref_num[q];
        if(w && w != nextn) continue;
        ref_vis[q] = 1;
        int stop = ref_dfs(q, depth + 1, w ? nextn + 1 : nextn, limit, found);
        ref_vis[q] = 0;
        if(stop) return 1;
    }
    return 0;
}
static int ref_count(const ZpGame *g, int limit){
    int start = -1, found = 0;
    for(int p = 0; p < ZP_CELLS; p++){ ref_num[p] = g->num[p]; if(g->num[p] == 1) start = p; }
    if(start < 0) return 0;
    memset(ref_vis, 0, sizeof ref_vis);
    ref_vis[start] = 1; ref_nodes = 0;
    ref_dfs(start, 1, 2, limit, &found);
    return found;
}

/* walk a solution found by the reference solver back into the game via zp_touch(),
 * i.e. play the board the way a finger would. */
static int ref_sol[ZP_CELLS], ref_sol_len;
static int cap_dfs(int cur, int depth, int nextn){
    ref_sol[depth - 1] = cur;
    if(depth == ZP_CELLS){ ref_sol_len = depth; return 1; }
    int r = cur / ZP_N, c = cur % ZP_N;
    int cand[4], n = 0;
    if(r > 0)        cand[n++] = cur - ZP_N;
    if(r < ZP_N - 1) cand[n++] = cur + ZP_N;
    if(c > 0)        cand[n++] = cur - 1;
    if(c < ZP_N - 1) cand[n++] = cur + 1;
    for(int i = 0; i < n; i++){
        int q = cand[i];
        if(ref_vis[q]) continue;
        int w = ref_num[q];
        if(w && w != nextn) continue;
        ref_vis[q] = 1;
        if(cap_dfs(q, depth + 1, w ? nextn + 1 : nextn)) return 1;
        ref_vis[q] = 0;
    }
    return 0;
}
static int ref_solution(const ZpGame *g){
    int start = -1;
    for(int p = 0; p < ZP_CELLS; p++){ ref_num[p] = g->num[p]; if(g->num[p] == 1) start = p; }
    if(start < 0) return 0;
    memset(ref_vis, 0, sizeof ref_vis);
    ref_vis[start] = 1; ref_sol_len = 0;
    return cap_dfs(start, 1, 2);
}

int main(void){
    printf("== zip ==\n");

    /* ---- a generated board is well formed ---- */
    ZpGame g; zp_new(&g, 20260724u);
    CK(g.nnum >= 2 && g.nnum <= ZP_MAXNUM, "waypoint count in range");
    int seen[ZP_MAXNUM + 2]; memset(seen, 0, sizeof seen);
    int numbered = 0;
    for(int p = 0; p < ZP_CELLS; p++) if(g.num[p]){ numbered++; seen[g.num[p]]++; }
    CK(numbered == g.nnum, "one cell per waypoint number");
    int contiguous = 1;
    for(int i = 1; i <= g.nnum; i++) if(seen[i] != 1) contiguous = 0;
    CK(contiguous, "numbers are exactly 1..nnum with no gaps or dupes");
    CK(g.state == ZP_PLAY, "starts in play");
    CK(g.plen == 1 && g.path[0] == zp_start_cell(&g), "path starts on the '1' cell");
    CK(zp_remaining(&g) == ZP_CELLS - 1, "35 cells left to cover");
    CK(!zp_solved(&g), "not solved at start");

    /* ---- solvable, and the module agrees with an independent solver ---- */
    CK(ref_count(&g, 2) >= 1, "the board is solvable (reference solver)");
    CK(zp_count_solutions(&g, 2) == ref_count(&g, 2), "module and reference agree on the count");

    /* ---- determinism ---- */
    ZpGame g2; zp_new(&g2, 20260724u);
    CK(memcmp(g.num, g2.num, ZP_CELLS) == 0, "same seed -> same board");
    ZpGame g3; zp_new(&g3, 777u);
    CK(memcmp(g.num, g3.num, ZP_CELLS) != 0, "different seed -> different board");

    /* ---- drawing rules ---- */
    ZpGame p; zp_new(&p, 4242u);
    int s = zp_start_cell(&p);
    CK(zp_head(&p) == s, "head is the start cell");
    CK(!zp_extend(&p, s), "cannot re-enter the cell you are on");
    /* a diagonal is not a legal single step */
    int diag = -1;
    for(int q = 0; q < ZP_CELLS; q++){
        int dr = q / ZP_N - s / ZP_N, dc = q % ZP_N - s % ZP_N;
        if((dr == 1 || dr == -1) && (dc == 1 || dc == -1)){ diag = q; break; }
    }
    CK(diag >= 0 && !zp_can_extend(&p, diag), "a diagonal is not adjacent");
    /* a far cell is not a legal single step */
    int far = (s + ZP_CELLS / 2) % ZP_CELLS;
    CK(!zp_adjacent(s, far) ? !zp_can_extend(&p, far) : 1, "a distant cell cannot be entered");

    /* the next number cannot be skipped: waypoint 3+ is refused while 2 is unvisited */
    int blocked = 1;
    for(int q = 0; q < ZP_CELLS; q++)
        if(p.num[q] >= 3 && zp_adjacent(zp_head(&p), q) && zp_can_extend(&p, q)) blocked = 0;
    CK(blocked, "a later number cannot be entered out of order");
    CK(zp_next_num(&p) == 2, "next expected number is 2 at the start");

    /* ---- play the reference solution through zp_touch(), one cell at a time ---- */
    CK(ref_solution(&p), "reference solver found a solution to play");
    int played = 1, order_ok = 1, expect = 2;
    for(int i = 1; i < ref_sol_len; i++){
        if(!zp_touch(&p, ref_sol[i])){ played = 0; break; }
        int w = p.num[ref_sol[i]];
        if(w){ if(w != expect) order_ok = 0; expect++; }
    }
    CK(played, "every step of a real solution is accepted");
    CK(order_ok, "numbers were hit in ascending order");
    CK(p.plen == ZP_CELLS, "the finished path covers all 36 cells");
    CK(zp_solved(&p) && p.state == ZP_SOLVED, "the board reports solved");
    /* no cell used twice */
    int used[ZP_CELLS]; memset(used, 0, sizeof used); int dup = 0;
    for(int i = 0; i < p.plen; i++){ if(used[p.path[i]]++) dup = 1; }
    CK(!dup, "no cell appears twice on the path");
    int steps_ok = 1;
    for(int i = 1; i < p.plen; i++) if(!zp_adjacent(p.path[i - 1], p.path[i])) steps_ok = 0;
    CK(steps_ok, "consecutive path cells are orthogonally adjacent");

    /* ---- retrace / undo / clear ---- */
    int wasl = p.plen;
    CK(zp_undo(&p) && p.plen == wasl - 1, "undo drops one cell");
    CK(!p.on[ref_sol[wasl - 1]], "the undone cell is free again");
    CK(p.state == ZP_PLAY, "undo un-solves the board");
    CK(zp_touch(&p, ref_sol[10]) && p.plen == 11, "touching a path cell rewinds to it");
    CK(zp_head(&p) == ref_sol[10], "head follows the rewind");
    CK(zp_touch(&p, ref_sol[11]) && p.plen == 12, "you can redraw after a rewind");
    zp_clear(&p);
    CK(p.plen == 1 && zp_head(&p) == zp_start_cell(&p), "clear rewinds to the '1'");
    CK(!zp_undo(&p), "undo cannot pop the forced start");

    /* A diagonal drag bridges through a shared free neighbour (the finger case).
     * Scan seeds for a board whose start has a legal diagonal -- on some boards the
     * only diagonals are blocked by a number out of turn, which is correct but
     * proves nothing, so the test must not depend on which seed it drew. */
    ZpGame d; int br = -1, bridged = 0;
    for(uint32_t sd = 900u; sd < 940u && !bridged; sd++){
        zp_new(&d, sd);
        int h = zp_head(&d);
        for(int q = 0; q < ZP_CELLS; q++){
            int dr = q / ZP_N - h / ZP_N, dc = q % ZP_N - h % ZP_N;
            if((dr == 1 || dr == -1) && (dc == 1 || dc == -1) && !d.on[q]){
                if(zp_touch(&d, q)){ br = q; bridged = 1; break; }
            }
        }
    }
    CK(bridged, "a diagonal drag bridges through one middle cell");
    CK(d.plen == 3 && (int)d.path[2] == br, "the bridge lands on the dragged-to cell");
    CK(zp_adjacent(d.path[0], d.path[1]) && zp_adjacent(d.path[1], d.path[2]),
       "the bridged path is still made of single steps");
    CK(!zp_adjacent(d.path[0], br), "and the target really was two steps away");

    /* ---- the generator over many seeds: always solvable, mostly unique, fast ---- */
    int nsolv = 0, nuniq = 0, minw = 99, maxw = 0;
    const int N = 40;
    clock_t t0 = clock();
    for(int i = 0; i < N; i++){
        ZpGame b; zp_new(&b, 1000u + 7919u * (uint32_t)i);
        int c1 = zp_count_solutions(&b, 2);
        if(c1 >= 1) nsolv++;
        if(c1 == 1) nuniq++;
        if(b.nnum < minw) minw = b.nnum;
        if(b.nnum > maxw) maxw = b.nnum;
        if(zp_start_cell(&b) < 0) nsolv = -1;
    }
    double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC / N;
    printf("  %d boards: %d solvable, %d unique, waypoints %d..%d, %.1f ms each\n",
           N, nsolv, nuniq, minw, maxw, ms);
    CK(nsolv == N, "every generated board is solvable");
    CK(nuniq >= N * 9 / 10, "at least 90% of boards have exactly one solution");
    CK(maxw <= ZP_MAXNUM, "waypoint count stays within ZP_MAXNUM");
    /* the device is ~15x slower than this host; keep New well inside a tap. */
    CK(ms < 60.0, "generation averages under 60 ms on the host");

    printf(fails ? "== zip: %d FAILURE(S) ==\n" : "== zip: all passed ==\n", fails);
    return fails ? 1 : 0;
}
