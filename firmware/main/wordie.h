/* wordie.h -- pure Wordie game state (no LVGL), for the Games app.
 *
 * A five-letter, six-guess word game. Logic only: the answer bank, deterministic
 * daily/seeded answer selection, letter entry, and Wordle-style two-pass scoring
 * (correct / present / absent, with correct duplicate handling). The LVGL view --
 * the guess grid and the on-screen keyboard, both drawn mono on a 1-bpp canvas --
 * lives in ui.c. Host-testable (sim/tests/wordie_test.c). */
#ifndef WORDIE_H
#define WORDIE_H
#include <stdint.h>

#define WD_LEN  5
#define WD_ROWS 6

/* per-letter score of a submitted guess */
enum { WD_ABSENT = 0, WD_PRESENT = 1, WD_CORRECT = 2 };

/* keyboard key state (max score ever seen for that letter; CORRECT wins) */
enum { WK_UNUSED = 0, WK_ABSENT = 1, WK_PRESENT = 2, WK_CORRECT = 3 };

/* game state */
enum { WD_PLAY = 0, WD_WON = 1, WD_LOST = 2 };

typedef struct {
    char    answer[WD_LEN + 1];
    char    guess[WD_ROWS][WD_LEN + 1];    /* submitted rows */
    uint8_t mark[WD_ROWS][WD_LEN];         /* per-letter WD_* for submitted rows */
    uint8_t nrows;                         /* number of submitted guesses */
    uint8_t cur;                           /* letters typed in the in-progress row */
    char    row[WD_LEN + 1];               /* the in-progress row */
    uint8_t key[26];                       /* WK_* per letter A..Z, for the keyboard */
    uint8_t state;                         /* WD_PLAY / WD_WON / WD_LOST */
} WdGame;

/* number of words in the answer bank (daily index is taken modulo this). */
int wd_nbank(void);

/* start today's puzzle: answer = bank[day mod wd_nbank()] (day = a day index, e.g.
 * days since the epoch), so everyone with the same date gets the same word. */
void wd_daily(WdGame *g, long day);

/* start a fresh puzzle from an arbitrary seed (the "New" button). */
void wd_random(WdGame *g, uint32_t seed);

/* append a letter to the in-progress row (case-insensitive; non-letters ignored).
 * Returns 1 if a letter was added, 0 otherwise (row full / game over / bad char). */
int wd_addch(WdGame *g, char c);

/* remove the last letter of the in-progress row. Returns 1 if one was removed. */
int wd_del(WdGame *g);

/* submit the in-progress row. No-op (returns 0) unless it holds WD_LEN letters and
 * the game is still in play; otherwise scores it, updates the keyboard states, may
 * set WD_WON / WD_LOST, clears the in-progress row, and returns 1. */
int wd_enter(WdGame *g);

#endif
