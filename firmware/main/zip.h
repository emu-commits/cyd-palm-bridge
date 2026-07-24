/* zip.h -- pure Zip game state (no LVGL), for the Games app.
 *
 * Zip is a one-line path puzzle on a 6x6 grid. A handful of cells carry numbers
 * 1..N; you draw a SINGLE continuous path of orthogonal steps that
 *   - starts on 1,
 *   - hits the numbers in ascending order, and
 *   - passes through EVERY cell exactly once.
 * (A Hamiltonian path with ordered waypoints.) The generator is seeded and
 * deterministic, and picks waypoints until the puzzle has exactly ONE solution,
 * so every board is fair and reproducible -- and host-testable.
 *
 * The out-of-order rule is enforced as you draw (like the original game): you
 * simply cannot step onto a number before its turn. That makes "solved" fall out
 * of coverage alone -- if all 36 cells are on the path, the order was already
 * legal. Retracing is a first-class move: touching a cell already on the path
 * rewinds to it.
 *
 * All state is fixed-size POD (~120 bytes, no allocation, no recursion in the
 * play path) because the device has no PSRAM. The generator's search recurses to
 * at most 36 frames. The LVGL view (grid + path + buttons on ONE 1-bpp canvas)
 * lives in ui.c; the logic is gated by sim/tests/zip_test.c.
 */
#ifndef ZIP_H
#define ZIP_H
#include <stdint.h>

#define ZP_N      6                    /* grid is ZP_N x ZP_N */
#define ZP_CELLS  (ZP_N * ZP_N)
/* Waypoint-count ceiling. The generator does not pick this number -- it falls out
 * of minimizing each board (see zip.c). Measured over 2000 seeds: 5..12 numbers,
 * centred on 7-9, which is the same feel as the original game. 16 leaves headroom
 * for an unlucky seed; a board above it would simply be an easier one. */
#define ZP_MAXNUM 16

enum { ZP_PLAY = 0, ZP_SOLVED = 1 };

typedef struct {
    uint8_t  num[ZP_CELLS];   /* 0 = plain cell, else its waypoint number 1..nnum */
    uint8_t  path[ZP_CELLS];  /* the drawn path as cell indices; path[0] = the '1' */
    uint8_t  on[ZP_CELLS];    /* 1 if the cell is on the path (O(1) hit test)      */
    uint8_t  nnum;            /* how many numbered cells                            */
    uint8_t  plen;            /* path length in cells (>= 1 once started)           */
    uint8_t  state;           /* ZP_PLAY / ZP_SOLVED                                */
    uint8_t  pad;
    uint32_t seed;
} ZpGame;

/* build a fresh puzzle from `seed` (deterministic) and reset the path to just the
 * '1' cell. Always yields a solvable board; aims for a unique solution. */
void zp_new(ZpGame *g, uint32_t seed);

/* rewind the path back to the '1' cell (the Clear button). */
void zp_clear(ZpGame *g);

/* cell index of waypoint 1 (the forced start), or -1 if the board has none. */
int zp_start_cell(const ZpGame *g);

/* the cell the path currently ends on, or -1 if the path is empty. */
int zp_head(const ZpGame *g);

/* are cell indices `a` and `b` orthogonally adjacent on the grid? */
int zp_adjacent(int a, int b);

/* the waypoint number the path is allowed to enter next (nnum+1 once all are
 * visited, i.e. "no more numbers to hit"). */
int zp_next_num(const ZpGame *g);

/* may the path step from its head onto `cell`? (unvisited, adjacent, and -- if
 * numbered -- next in sequence). */
int zp_can_extend(const ZpGame *g, int cell);

/* append `cell` to the path if zp_can_extend() allows it. Returns 1 if the board
 * changed. */
int zp_extend(ZpGame *g, int cell);

/* drop the last cell of the path (never the forced '1' start). 1 if it changed. */
int zp_undo(ZpGame *g);

/* rewind the path so it ends on `cell` (must already be on the path). 1 if it
 * changed. */
int zp_truncate(ZpGame *g, int cell);

/* the single entry point the view needs for a touch on `cell`:
 *   - on the path already -> rewind to it,
 *   - a legal next step   -> extend,
 *   - two steps away with a legal cell between (a fast or diagonal drag) ->
 *     extend through that middle cell,
 *   - otherwise ignored.
 * Returns 1 if the board changed. */
int zp_touch(ZpGame *g, int cell);

/* 1 when every cell is on the path (given the order rule is enforced on entry,
 * that is a correct solve). */
int zp_solved(const ZpGame *g);

/* how many cells are not on the path yet. */
int zp_remaining(const ZpGame *g);

/* 1 if `g` holds a structurally sane game: numbers 1..nnum exactly once each, a
 * path of in-range cells that starts on the '1' and steps one cell at a time, and an
 * `on[]` mask that agrees with it. Used to vet a loaded save file -- a corrupt blob
 * would otherwise index `on[]` with a path byte up to 255. */
int zp_valid(const ZpGame *g);

/* count full solutions of the puzzle's number layout, stopping at `limit` (so
 * `== 1` is a cheap uniqueness check). Ignores the player's current path. Used by
 * the generator and the host gate. */
int zp_count_solutions(const ZpGame *g, int limit);

#endif
