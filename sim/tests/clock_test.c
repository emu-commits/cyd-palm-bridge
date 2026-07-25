/* clock_test.c -- host gate for the pausable game play clock (playclock.h).
 *
 * The bug this locks down: the first game timers stored one "started at" epoch, so
 * closing the app and coming back an hour later showed an hour of "play" time. The
 * clock must only advance while the game screen is open. Every call takes `now`, so
 * the whole thing is testable without sleeping. */
#include <stdio.h>
#include "playclock.h"

static int fails = 0;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } else printf("  ok: %s\n",(m)); }while(0)

int main(void){
    printf("== playclock ==\n");
    const uint32_t T0 = 1700000000u;             /* an arbitrary epoch */

    /* a fresh clock reads zero and is not running */
    PlayClock k; pc_reset(&k);
    CK(pc_secs(&k, T0) == 0, "fresh clock reads 0");
    CK(!k.on && !k.run, "fresh clock is not started");

    /* it does NOT start counting until the first move */
    CK(pc_secs(&k, T0 + 500) == 0, "un-started clock ignores wall time");

    /* start -> counts live */
    pc_start(&k, T0);
    CK(k.on, "start marks the clock on");
    CK(pc_secs(&k, T0 + 10) == 10, "counts while running");

    /* a second start is a no-op (callers can say 'a move happened' every time) */
    pc_start(&k, T0 + 10);
    CK(pc_secs(&k, T0 + 20) == 20, "re-start does not reset a running clock");

    /* THE FIX: pause banks the segment, and time passing while paused is free */
    pc_pause(&k, T0 + 30);
    CK(pc_secs(&k, T0 + 30) == 30, "pause banks the elapsed segment");
    CK(pc_secs(&k, T0 + 3600) == 30, "a paused clock ignores an hour away");
    pc_pause(&k, T0 + 4000);
    CK(pc_secs(&k, T0 + 4000) == 30, "pausing twice does not double-bank");

    /* resume adds a NEW segment on top of the banked total */
    pc_resume(&k, T0 + 4000);
    CK(pc_secs(&k, T0 + 4005) == 35, "resume continues from the banked total");
    pc_resume(&k, T0 + 4005);
    CK(pc_secs(&k, T0 + 4010) == 40, "re-resume does not restart the segment");

    /* many open/close cycles accumulate only the open time */
    PlayClock c; pc_reset(&c); pc_start(&c, T0);
    uint32_t t = T0;
    for(int i = 0; i < 5; i++){
        t += 7;  pc_pause(&c, t);                /* 7 s of play  */
        t += 900; pc_resume(&c, t);              /* 15 min away  */
    }
    pc_pause(&c, t);
    CK(pc_secs(&c, t + 99999) == 35, "5 sessions of 7 s == 35 s, away time excluded");

    /* stop freezes for good: later resumes are ignored */
    pc_stop(&k, T0 + 4010);
    CK(pc_secs(&k, T0 + 4010) == 40, "stop keeps the final total");
    pc_resume(&k, T0 + 5000);
    CK(pc_secs(&k, T0 + 9999) == 40, "a stopped clock cannot be resumed");
    CK(k.done, "stop marks the clock done");

    /* persistence: a snapshot is always PAUSED, so a reboot can't charge for the
     * time the device was off. The screen resumes it explicitly on open. */
    PlayClock live; pc_reset(&live); pc_start(&live, T0);
    PlayClock saved = pc_snapshot(&live, T0 + 12);
    CK(saved.run == 0, "a snapshot is paused");
    CK(pc_secs(&saved, T0 + 100000) == 12, "a snapshot holds its total while off");
    CK(saved.on && !saved.done, "a snapshot keeps started/not-done");
    pc_resume(&saved, T0 + 100000);              /* device back on, screen reopened */
    CK(pc_secs(&saved, T0 + 100005) == 17, "a restored clock resumes from its total");
    CK(pc_secs(&live, T0 + 12) == 12, "snapshotting does not disturb the live clock");

    /* a snapshot of a finished game stays finished and frozen */
    PlayClock fin; pc_reset(&fin); pc_start(&fin, T0); pc_stop(&fin, T0 + 60);
    PlayClock fs = pc_snapshot(&fin, T0 + 900);
    pc_resume(&fs, T0 + 900);
    CK(pc_secs(&fs, T0 + 9000) == 60, "a finished game's time survives a reload frozen");

    /* a clock that never started stays zero across a save/restore cycle */
    PlayClock nz; pc_reset(&nz);
    PlayClock ns = pc_snapshot(&nz, T0);
    pc_resume(&ns, T0);
    CK(pc_secs(&ns, T0 + 500) == 0, "an unstarted clock does not start on reload");

    /* a clock stepped backwards (NTP/timezone fix mid-game) never goes negative */
    PlayClock b; pc_reset(&b); pc_start(&b, T0 + 100);
    CK(pc_secs(&b, T0) == 0, "a backwards wall clock reads 0, not a huge number");
    pc_pause(&b, T0);
    CK(pc_secs(&b, T0) == 0, "pausing on a backwards clock banks 0");

    printf(fails ? "== playclock: %d FAILURE(S) ==\n" : "== playclock: all passed ==\n", fails);
    return fails ? 1 : 0;
}
