/* minesweeper.h -- pure Minesweeper game state (no LVGL), for the Games app.
 *
 * Board logic only: mine placement (deterministic from a seed, so a "daily" board
 * is reproducible and host-ttestable), first-tap safety, flood-reveal of empty
 * regions, flagging, and win/lose detection. The LVGL view lives in ui.c. */
#ifndef MINESWEEPER_H
#define MINESWEEPER_H
#include <stdint.h>

#define MS_MAXW 10
#define MS_MAXH 10

/* per-cell flags packed in one byte */
enum { MS_MINE = 1, MS_REVEALED = 2, MS_FLAG = 4 };

enum { MS_PLAY, MS_WON, MS_LOST };

typedef struct {
    uint8_t w, h;
    uint8_t mines;
    uint8_t state;                 /* MS_PLAY / MS_WON / MS_LOST */
    uint8_t first;                 /* 1 until the first reveal (mines placed then) */
    uint32_t seed;                 /* RNG seed used to place mines */
    uint8_t cell[MS_MAXW * MS_MAXH];
    int8_t  adj[MS_MAXW * MS_MAXH];/* adjacent mine count, valid after placement */
} MsGame;

/* start a new game (w<=MS_MAXW, h<=MS_MAXH). Mines are NOT placed yet -- they're
 * laid down on the first reveal so the first tap is always safe. */
void ms_new(MsGame *g, int w, int h, int mines, uint32_t seed);

/* reveal (r,c). On the first reveal, mines are placed avoiding (r,c) and its
 * neighbours. Flood-reveals connected zero-adjacency cells. Returns MS_LOST if a
 * mine was hit, else the current state (MS_PLAY / MS_WON). No-op on flagged/known
 * cells or once the game is over. */
int ms_reveal(MsGame *g, int r, int c);

/* toggle a flag on an un-revealed cell (no-op once the game is over). */
void ms_flag(MsGame *g, int r, int c);

/* count of flags currently placed (for the "mines left" readout). */
int ms_flags(const MsGame *g);

static inline uint8_t ms_at(const MsGame *g, int r, int c){ return g->cell[r*g->w + c]; }
static inline int8_t  ms_adj(const MsGame *g, int r, int c){ return g->adj[r*g->w + c]; }

#endif
