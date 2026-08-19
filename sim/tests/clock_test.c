/* clock_test.c -- host gate for the pausable game play clock (playclock.h).
 *
 * The bug this locks down: the first game timers stored one "started at" epoch, so
 * closing the app and coming back an hour later showed an hour of "play" time. The
 * clock must only advance while the game screen is open. Every call takes `now`, so
 * the whole thing is testable without sleeping. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "playclock.h"

/* clock.c's drift meter, exposed by -DCLOCK_TEST_HOOKS. The arithmetic is the
 * part that can be wrong silently -- a flipped sign would report a fast clock as
 * slow and send us shopping for the wrong fix. */
void drift_line(char *out, int cap, int64_t corr_us, long long span_s, unsigned lost);

static int fails = 0;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } else printf("  ok: %s\n",(m)); }while(0)

#define HAS(hay,needle) (strstr((hay),(needle)) != NULL)

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

    printf("== clock drift meter ==\n");
    {
        char m[168];
        const long long DAY = 86400;

        /* A clock that ran SLOW is behind, so SNTP moves it FORWARD: the
         * correction is positive. 8.64 s over a day is exactly 100 ppm, which
         * makes this the one case worth checking by hand. */
        drift_line(m, sizeof m, 8640000LL, DAY, 0);
        CK(HAS(m, "SLOW"),          "a positive correction means the clock ran slow");
        CK(HAS(m, "100.00 ppm"),    "8.64 s/day is 100 ppm");
        CK(HAS(m, "8.64 s/day"),    "and reads back as 8.64 s/day");
        CK(HAS(m, "+8640 ms"),      "the correction is shown in ms, signed");
        CK(HAS(m, "24h00m"),        "the interval is shown as hours and minutes");

        /* the mirror case: a fast clock is ahead, SNTP winds it back */
        drift_line(m, sizeof m, -8640000LL, DAY, 0);
        CK(HAS(m, "FAST"),          "a negative correction means the clock ran fast");
        CK(HAS(m, "100.00 ppm"),    "the rate is reported unsigned, after the word");
        CK(HAS(m, "-8640 ms"),      "the correction keeps its sign");
        CK(!HAS(m, "--100"),        "no double negative leaks into the rate");

        /* the sub-ppm end: this is the resolution that decides "no RTC needed" */
        drift_line(m, sizeof m, 86400LL, DAY, 0);     /* 86.4 ms/day = 1 ppm */
        CK(HAS(m, "1.00 ppm"),      "1 ppm resolves");
        CK(HAS(m, "0.08 s/day"),    "and shows as 0.08 s/day");

        /* an interval that contains a power loss is NOT a drift sample */
        drift_line(m, sizeof m, 3600000000LL, DAY, 1);
        CK(HAS(m, "not drift"),     "a sample spanning an outage says so");
        CK(!HAS(m, "ppm"),          "and quotes no rate at all");
        CK(HAS(m, "1 time(s)"),     "it reports how many times the clock stopped");

        /* too short to mean anything: SNTP's own error would dominate */
        drift_line(m, sizeof m, 40000LL, 300, 0);
        CK(HAS(m, "too short"),     "a five-minute interval is rejected");
        CK(!HAS(m, "ppm"),          "and quotes no rate");
        drift_line(m, sizeof m, 40000LL, 600, 0);
        CK(HAS(m, "ppm"),           "ten minutes is the first interval that counts");

        /* a real-world-shaped sample: a few seconds across most of a day */
        drift_line(m, sizeof m, 4812000LL, 19*3600 + 22*60, 0);
        CK(HAS(m, "19h22m"),        "hours and minutes format with a leading zero");
        CK(HAS(m, "SLOW"),          "and it reads as a slow clock");

        /* an hour-boundary interval must not print 60 minutes */
        drift_line(m, sizeof m, 1000000LL, 7200, 0);
        CK(HAS(m, "2h00m"),         "an exact two hours is 2h00m, not 1h60m");

        /* nothing here may overflow or truncate the buffer */
        drift_line(m, sizeof m, 30LL*DAY*1000000, 365*DAY, 0);
        CK(strlen(m) < sizeof m - 1, "a year-long interval still fits the line");
        CK(HAS(m, "ppm"),           "and still produces a rate");
    }

    printf(fails ? "== clock: %d FAILURE(S) ==\n" : "== clock: all passed ==\n", fails);
    return fails ? 1 : 0;
}
