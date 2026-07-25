/* playclock.h -- a PAUSABLE play timer, shared by the Games apps (Mines, Sudoku,
 * Zip).
 *
 * The first cut of the game timers stored a single "started at" epoch and showed
 * `now - start`. That kept counting while the game was closed: leave a half-solved
 * Sudoku, come back after lunch, and the clock read 47:00. A play timer should
 * measure time spent PLAYING, so this banks elapsed seconds into `accum` whenever
 * the screen closes and starts a fresh segment when it reopens.
 *
 * Pure and clock-injected on purpose: every call takes `now` (UNIX epoch seconds)
 * rather than calling time(), so the whole state machine is deterministic and
 * host-testable (sim/tests/clock_test.c). ui.c passes time(NULL).
 *
 * It is plain POD, so it persists inside a game's save blob. Save a
 * pc_snapshot(): that banks the live segment and leaves the clock PAUSED, so a
 * reboot (or a battery pull) can never charge the player for time the device was
 * off -- the screen resumes it explicitly on open.
 */
#ifndef PLAYCLOCK_H
#define PLAYCLOCK_H
#include <stdint.h>

typedef struct {
    uint32_t accum;    /* seconds banked from earlier segments                  */
    uint32_t run;      /* epoch the current segment resumed (0 = paused)        */
    uint8_t  on;       /* 1 once started; survives pauses (0 = never started)   */
    uint8_t  done;     /* 1 when the game ended: frozen, resume is a no-op      */
    uint8_t  pad[2];   /* keep the struct a tidy 12 bytes in save files          */
} PlayClock;

static inline void pc_reset(PlayClock *k){
    k->accum = 0; k->run = 0; k->on = 0; k->done = 0; k->pad[0] = k->pad[1] = 0;
}

/* total play seconds: banked + the live segment (0 before the first start). */
static inline uint32_t pc_secs(const PlayClock *k, uint32_t now){
    uint32_t s = k->accum;
    if(k->run && now > k->run) s += now - k->run;   /* a backwards clock adds 0 */
    return s;
}

/* begin timing (first move of a game). Idempotent: a second call is a no-op, so
 * callers can just say "the player moved" without tracking whether it's the first. */
static inline void pc_start(PlayClock *k, uint32_t now){
    if(k->on) return;
    pc_reset(k);
    k->on = 1; k->run = now;
}

/* bank the live segment and stop counting (the screen is closing). */
static inline void pc_pause(PlayClock *k, uint32_t now){
    if(!k->run) return;
    k->accum = pc_secs(k, now);
    k->run = 0;
}

/* start a new segment (the screen reopened). No-op if never started, already
 * running, or the game is over. */
static inline void pc_resume(PlayClock *k, uint32_t now){
    if(!k->on || k->done || k->run) return;
    k->run = now;
}

/* the game ended: freeze for good. pc_secs() is stable from here on. */
static inline void pc_stop(PlayClock *k, uint32_t now){
    pc_pause(k, now);
    k->done = 1;
}

/* a paused copy for persistence (see the header comment). */
static inline PlayClock pc_snapshot(const PlayClock *k, uint32_t now){
    PlayClock s = *k;
    s.accum = pc_secs(k, now);
    s.run = 0;
    return s;
}

#endif
