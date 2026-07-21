/* sudoku.h -- pure Sudoku game state (no LVGL), for the Games app.
 *
 * A 9x9 Sudoku: a seeded generator (deterministic from a seed, so a puzzle is
 * reproducible and host-testable) that produces a puzzle with a UNIQUE solution,
 * plus placement, conflict detection, and solved detection. The clue cells are
 * fixed; the player fills the rest. The on-brand input is the Graffiti digit
 * strip; the LVGL view (grid + number pad on a 1-bpp canvas) lives in ui.c.
 * Host-testable (sim/tests/sudoku_test.c). */
#ifndef SUDOKU_H
#define SUDOKU_H
#include <stdint.h>

#define SD_N     9
#define SD_CELLS 81

enum { SD_PLAY = 0, SD_SOLVED = 1 };

typedef struct {
    uint8_t given[SD_CELLS];   /* clue value per cell (0 = not a clue) -- the fixed mask */
    uint8_t cell[SD_CELLS];    /* current value per cell (0 = empty); givens copied in */
    uint8_t state;             /* SD_PLAY / SD_SOLVED */
    uint32_t seed;             /* generator seed */
} SdGame;

/* generate a new puzzle from `seed`, trying to remove up to `holes` cells while
 * keeping the solution unique (fewer are removed if uniqueness would break). A
 * larger `holes` -> harder puzzle; ~45-51 gives a medium board (30-36 clues). */
void sd_new(SdGame *g, uint32_t seed, int holes);

/* is cell (r,c) a fixed clue (not editable)? */
int sd_is_given(const SdGame *g, int r, int c);

/* place `val` (1..9) at (r,c), or 0 to clear it. No-op on a clue cell or a bad
 * value. Updates the solved state. Returns 1 if the board changed, else 0. */
int sd_set(SdGame *g, int r, int c, int val);

/* 1 if the value at (r,c) is non-empty and duplicated in its row, column, or 3x3
 * box (a rule violation to flag in the UI); 0 otherwise. */
int sd_conflict(const SdGame *g, int r, int c);

/* count of empty (unfilled) cells -- 0 means the board is full. */
int sd_remaining(const SdGame *g);

/* 1 if every cell is filled and no cell conflicts (i.e. solved correctly). */
int sd_solved(const SdGame *g);

static inline uint8_t sd_at(const SdGame *g, int r, int c){ return g->cell[r*SD_N + c]; }

#endif
