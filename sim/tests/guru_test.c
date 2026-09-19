/* guru_test.c -- host gate for the pure Guru logic (firmware/main/guru.c).
 *
 * Guru's target is the number the user is asked to beat, and it is derived from
 * their own history rather than chosen, so the arithmetic has to be right before
 * a pixel is drawn. The two rules that make it fair are the ones most likely to
 * be broken by a later edit, and both are pinned here: today never counts toward
 * its own target, and a skipped day counts as a zero.
 *
 * Everything in guru.c takes its clock and zone as arguments, so this runs
 * identically on any host in any locale, with no sleeping and no /sdcard.
 *
 * Covers: the rolling window (including saturation past a week and gaps longer
 * than one), half-up rounding and the floor, day rollover across a negative zone,
 * streak arithmetic and its undo, a backwards clock, and per-day saturation. */
#include <stdio.h>
#include "guru.h"
#include "daycal.h"

static int fails = 0;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } else printf("  ok: %s\n",(m)); }while(0)

/* 2026-08-17 00:00:00 UTC, a Monday -- the same anchor coach_test.c uses. */
#define T0   1786924800u
#define HOUR 3600u
#define DAY  86400u

/* 09:00 UTC on day `d` after the anchor. */
#define AT(d) (T0 + (uint32_t)(d) * DAY + 9 * HOUR)

/* A state as if the user had checked off counts[0..n-1] on n consecutive local
 * days starting at the anchor -- so "today" for the caller is day n, still empty.
 * Empty days are rolled through rather than skipped, which exercises the rollover
 * path as well as the arithmetic. */
static GuruState hist(const int *counts, int n){
    GuruState s;
    guru_state_init(&s);
    for(int d = 0; d < n; d++){
        for(int i = 0; i < counts[d]; i++) guru_note_check(&s, AT(d), 0);
        if(counts[d] == 0) guru_roll(&s, AT(d), 0);
    }
    return s;
}

int main(void){
    printf("== guru ==\n");

    /* ------------------------------------------------------------- a fresh user */
    {
        GuruState s;
        guru_state_init(&s);
        CK(guru_target(&s, AT(0), 0) == GU_MIN_TARGET, "a fresh user's target is the floor");
        CK(guru_today_n(&s, AT(0), 0) == 0,            "a fresh user has done nothing today");
        CK(guru_window_days(&s, AT(0), 0) == 0,        "a fresh user has no history to average");
        CK(guru_streak_now(&s, AT(0), 0) == 0,         "a fresh user has no streak");
        CK(!guru_met_today(&s, AT(0), 0),              "and has not met the floor yet");

        /* the rule that keeps the treadmill from speeding up while you run on it */
        for(int i = 0; i < 5; i++) guru_note_check(&s, AT(0), 0);
        CK(guru_today_n(&s, AT(0), 0) == 5,            "five checks land on today");
        CK(guru_target(&s, AT(0), 0) == GU_MIN_TARGET, "today is excluded from its own target");
        CK(guru_met_today(&s, AT(0), 0),               "five clears a target of one");
        CK(guru_streak_now(&s, AT(0), 0) == 1,         "the first active day is a streak of one");

        /* ...and tomorrow it lands */
        CK(guru_target(&s, AT(1), 0) == 5,             "yesterday's five becomes today's target");
        CK(guru_window_days(&s, AT(1), 0) == 1,        "drawn from exactly one day of history");
        CK(guru_today_n(&s, AT(1), 0) == 0,            "the new day starts empty");
        CK(!guru_met_today(&s, AT(1), 0),              "an empty day has not met it");
        CK(guru_streak_now(&s, AT(1), 0) == 1,         "the streak survives until the day is over");
    }

    /* ------------------------------------------------------- rounding, half UP */
    {
        int a[] = { 3, 2 };            /* 2.5 */
        int b[] = { 5, 2 };            /* 3.5 */
        int c[] = { 1, 1, 2 };         /* 1.33 */
        int d[] = { 0, 0, 0 };         /* 0    */
        GuruState sa = hist(a, 2), sb = hist(b, 2), sc = hist(c, 3), sd = hist(d, 3);
        CK(guru_target(&sa, AT(2), 0) == 3,            "an average of 2.5 rounds up to 3");
        CK(guru_target(&sb, AT(2), 0) == 4,            "an average of 3.5 rounds up to 4");
        CK(guru_target(&sc, AT(3), 0) == 1,            "an average of 1.33 rounds down to 1");
        CK(guru_target(&sd, AT(3), 0) == GU_MIN_TARGET,"an average of zero is held at the floor");
        CK(guru_window_days(&sd, AT(3), 0) == 3,       "three empty days are still three days of data");
    }

    /* -------------------------------------------------- gaps pull the target down */
    {
        int counts[] = { 5, 2 };
        GuruState s = hist(counts, 2);
        CK(guru_target(&s, AT(2), 0) == 4,             "two days of 5 and 2 ask for 4");

        /* two days go by untouched. They are real days that happened to be empty,
         * so they belong in the average -- coming back should not face the bar
         * that was set before the lapse. (5+2+0+0)/4 = 1.75 -> 2. */
        CK(guru_target(&s, AT(4), 0) == 2,             "days you skipped count as zeros");
        CK(guru_window_days(&s, AT(4), 0) == 4,        "and widen the window as they pass");
        CK(guru_streak_now(&s, AT(4), 0) == 0,         "a streak does not survive a skipped day");

        /* reading the target must not depend on whether anyone rolled first */
        CK(guru_roll(&s, AT(4), 0) == 3,               "rolling forward reports the days advanced");
        CK(guru_roll(&s, AT(4), 0) == 0,               "rolling again the same day is a no-op");
        CK(guru_target(&s, AT(4), 0) == 2,             "rolling does not move the target");
        CK(guru_today_n(&s, AT(4), 0) == 0,            "nor invent checks on the day rolled into");

        guru_note_check(&s, AT(4), 0);
        CK(guru_streak_now(&s, AT(4), 0) == 1,         "a check after a gap starts a new streak");
    }

    /* ------------------------------------------- a gap longer than the whole window */
    {
        int counts[] = { 4, 4, 4 };
        GuruState s = hist(counts, 3);
        CK(guru_target(&s, AT(3), 0) == 4,             "three steady days ask for four");
        CK(guru_target(&s, AT(40), 0) == GU_MIN_TARGET,"a month away resets to the floor");
        CK(guru_roll(&s, AT(40), 0) == GU_WIN,         "a long gap is reported as one full window");
        CK(guru_window_total(&s, AT(40), 0) == 0,      "and leaves nothing in the window");
    }

    /* ------------------------------------------------------- the window saturates */
    {
        GuruState s;
        guru_state_init(&s);
        for(int d = 0; d < 10; d++)
            for(int i = 0; i < 3; i++) guru_note_check(&s, AT(d), 0);

        CK(guru_window_days(&s, AT(10), 0) == GU_WIN - 1, "the average never spans more than a week");
        CK(guru_target(&s, AT(10), 0) == 3,               "a steady three a day asks for three");
        CK(guru_window_total(&s, AT(10), 0) == 3 * (GU_WIN - 1), "the window total spans the week");
        CK(guru_streak_now(&s, AT(10), 0) == 10,          "ten consecutive days is a streak of ten");
        CK(guru_streak_now(&s, AT(11), 0) == 0,           "and it dies on the second idle day");

        /* the clock can jump backwards -- the CYD restores its epoch from NVS at
         * boot, so a rewind is far more likely to be a bad clock than a real day.
         * It must not be allowed to erase a week of history. */
        int32_t before = s.last_day;
        CK(guru_roll(&s, AT(5), 0) == 0,                  "a backwards clock rolls nothing");
        CK(s.last_day == before,                          "and does not rewind the stored day");
        CK(guru_window_total(&s, AT(10), 0) == 3 * (GU_WIN - 1), "the ring is intact after it");
    }

    /* ------------------------------------------------------------ undoing a check */
    {
        GuruState s;
        guru_state_init(&s);
        guru_note_check(&s, AT(0), 0);
        guru_note_check(&s, AT(1), 0);
        CK(guru_streak_now(&s, AT(1), 0) == 2,         "two days running is a streak of two");
        CK(s.best_streak == 2,                         "and the best rises with it");

        CK(guru_note_uncheck(&s, AT(1), 0) == 0,       "un-checking the last one empties the day");
        CK(guru_streak_now(&s, AT(1), 0) == 1,         "and hands the streak back to yesterday");
        CK(s.best_streak == 2,                         "without lowering the high-water mark");
        CK(s.total_n == 1,                             "the lifetime count gives the check back too");

        CK(guru_note_uncheck(&s, AT(1), 0) == 0,       "un-checking an empty day does nothing");
        CK(guru_streak_now(&s, AT(1), 0) == 1,         "and does not take yesterday as well");

        /* undoing the only check there has ever been ends the streak outright */
        GuruState t;
        guru_state_init(&t);
        guru_note_check(&t, AT(0), 0);
        guru_note_uncheck(&t, AT(0), 0);
        CK(guru_streak_now(&t, AT(0), 0) == 0,         "undoing a lone check ends the streak");

        /* an undo that does not empty the day leaves the streak alone */
        GuruState u;
        guru_state_init(&u);
        guru_note_check(&u, AT(0), 0);
        guru_note_check(&u, AT(0), 0);
        CK(guru_note_uncheck(&u, AT(0), 0) == 1,       "one of two checks comes back off");
        CK(guru_streak_now(&u, AT(0), 0) == 1,         "and the day is still an active one");
    }

    /* ----------------------------------------------- local days in a negative zone */
    {
        /* 02:00 UTC is 22:00 the PREVIOUS day in EDT (-240). The check has to file
         * under that evening, and 09:00 UTC the same date has to be a new day. */
        GuruState s;
        guru_state_init(&s);
        guru_note_check(&s, T0 + 2 * HOUR, -240);
        guru_note_check(&s, T0 + 2 * HOUR, -240);
        CK(guru_today_n(&s, T0 + 2 * HOUR, -240) == 2, "a late EDT evening files under that evening");
        CK(cal_day_index(T0 + 2 * HOUR, -240) ==
           cal_day_index(T0 + 9 * HOUR, -240) - 1,     "and 09:00 UTC is the next EDT day");
        CK(guru_today_n(&s, T0 + 9 * HOUR, -240) == 0, "so the morning starts empty");
        CK(guru_target(&s, T0 + 9 * HOUR, -240) == 2,  "with last evening's two as the target");
    }

    /* ------------------------------------------------------------- edges and nulls */
    {
        GuruState s;
        guru_state_init(&s);
        for(int i = 0; i < GU_DAY_MAX + 10; i++) guru_note_check(&s, AT(0), 0);
        CK(guru_today_n(&s, AT(0), 0) == GU_DAY_MAX,   "a day's count saturates instead of wrapping");

        CK(guru_target(0, AT(0), 0) == GU_MIN_TARGET,  "a null state answers the floor, not a crash");
        CK(guru_roll(0, AT(0), 0) == 0,                "a null state rolls nothing");
        CK(guru_today_n(0, AT(0), 0) == 0,             "a null state has done nothing");
        CK(guru_note_check(0, AT(0), 0) == 0,          "a null state cannot be checked off");
        guru_state_init(0);                            /* must simply return */
        CK(1,                                          "initialising a null state is survivable");
    }

    printf(fails ? "== guru: %d FAILURE(S) ==\n" : "== guru: all passed ==\n", fails);
    return fails ? 1 : 0;
}
