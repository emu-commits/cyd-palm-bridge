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
#include <string.h>
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

    /* ---------------------------------------------------------------- the pool */
    {
        int n = guru_ntasks();
        CK(n > 20,                                     "the pool is deep enough not to repeat");
        CK(guru_task(-1) == 0 && guru_task(n) == 0,    "positions outside the pool read as nothing");

        /* Every id must be in range, unique, and non-zero: id-1 is a bit index in
         * the saved state, so a duplicate would silently tick two rows at once
         * and an id past the ceiling would tick nobody. */
        int seen[GU_TASK_MAX + 2];
        for(int i = 0; i < GU_TASK_MAX + 2; i++) seen[i] = 0;
        int bad_id = 0, dup = 0, bad_cat = 0, empty = 0, toolong = 0;
        for(int i = 0; i < n; i++){
            const GuruTask *t = guru_task(i);
            if(t->id < 1 || t->id > GU_TASK_MAX){ bad_id++; continue; }
            if(seen[t->id]++) dup++;
            if(t->cat >= GU_NCAT) bad_cat++;
            if(!t->name || !t->name[0] || !t->why || !t->why[0]) empty++;
            /* the table column is ~194px of a ~6px proportional font; past about
             * thirty characters a row starts clipping on the device. */
            if(strlen(t->name) > 30) toolong++;
        }
        CK(bad_id  == 0, "every task id is inside the bitmap's ceiling");
        CK(dup     == 0, "no two tasks share an id");
        CK(bad_cat == 0, "every task has a real category");
        CK(empty   == 0, "every task has both a name and a why");
        CK(toolong == 0, "no task name is too long for the row");

        /* lookup by id is the log's half of the contract */
        const GuruTask *first = guru_task(0);
        CK(guru_task_by_id(first->id) == first,        "a task is findable by its stable id");
        CK(guru_task_by_id(0) == 0,                    "id zero belongs to nobody");
        CK(guru_task_by_id(GU_TASK_MAX + 1) == 0,      "an id past the ceiling finds nothing");

        /* every category is represented, or the week analysis has a silent hole */
        int percat[GU_NCAT];
        for(int c = 0; c < GU_NCAT; c++) percat[c] = 0;
        for(int i = 0; i < n; i++) percat[guru_task(i)->cat]++;
        int emptycat = 0;
        for(int c = 0; c < GU_NCAT; c++){
            if(percat[c] == 0) emptycat++;
            if(!guru_cat_name(c)[0]) emptycat++;
        }
        CK(emptycat == 0, "every category has a name and at least one task");
        CK(guru_cat_name(GU_NCAT)[0] == 0, "one past the end names nothing");

        /* the record is frozen: the week analysis reads these back off SD */
        CK(sizeof(GuruRec) == 8, "a logged check is exactly eight bytes");
    }

    /* ------------------------------------------------------- ticking a real task */
    {
        GuruState s;
        guru_state_init(&s);
        int id1 = guru_task(0)->id, id2 = guru_task(1)->id;

        CK(!guru_is_checked(&s, id1, AT(0), 0),        "nothing is ticked on a fresh day");
        CK(guru_toggle(&s, id1, AT(0), 0) == 1,        "tapping a row ticks it");
        CK(guru_is_checked(&s, id1, AT(0), 0),         "and it reads back as ticked");
        CK(!guru_is_checked(&s, id2, AT(0), 0),        "without ticking its neighbour");
        CK(guru_today_n(&s, AT(0), 0) == 1,            "the day's count followed it");

        CK(guru_toggle(&s, id1, AT(0), 0) == 0,        "tapping again un-ticks it");
        CK(!guru_is_checked(&s, id1, AT(0), 0),        "and it reads back clear");
        CK(guru_today_n(&s, AT(0), 0) == 0,            "the count came back down with it");

        guru_toggle(&s, id1, AT(0), 0);
        guru_toggle(&s, id2, AT(0), 0);
        CK(guru_today_n(&s, AT(0), 0) == 2,            "two different rows are two checks");

        /* the list is today's, not a running total: tomorrow starts blank even if
         * nobody has rolled the state yet */
        CK(!guru_is_checked(&s, id1, AT(1), 0),        "tomorrow's list starts empty");
        CK(guru_target(&s, AT(1), 0) == 2,             "while yesterday's two set the target");
        guru_roll(&s, AT(1), 0);
        CK(!guru_is_checked(&s, id1, AT(1), 0),        "and rolling agrees with the reader");
        CK(guru_today_n(&s, AT(1), 0) == 0,            "with no checks carried over");

        /* an id nobody has cannot tick anything */
        CK(guru_toggle(&s, 0, AT(1), 0) == 0,          "id zero cannot be ticked");
        CK(guru_toggle(&s, GU_TASK_MAX + 1, AT(1), 0) == 0, "nor an id past the ceiling");
        CK(guru_today_n(&s, AT(1), 0) == 0,            "and neither moved the count");
        CK(guru_toggle(0, id1, AT(1), 0) == 0,         "a null state cannot be toggled");
    }

    /* ------------------------------------------------- the week, out of the ring */
    {
        GuruState s;
        guru_state_init(&s);
        /* three days on, two days off, two days on -- inside one window */
        int plan[7] = { 2, 3, 0, 0, 1, 4, 2 };
        for(int d = 0; d < 7; d++)
            for(int i = 0; i < plan[d]; i++) guru_note_check(&s, AT(d), 0);

        CK(guru_window_total(&s, AT(6), 0) == 12,      "the window totals the whole week");
        CK(guru_window_days_active(&s, AT(6), 0) == 5, "and counts only the days with something on them");
        CK(guru_window_best(&s, AT(6), 0) == 4,        "and remembers the biggest day");

        /* an empty state must not claim a week of nothing is a week of something */
        GuruState e;
        guru_state_init(&e);
        CK(guru_window_days_active(&e, AT(0), 0) == 0, "a fresh user has no active days");
        CK(guru_window_best(&e, AT(0), 0) == 0,        "and no best day");
        CK(guru_window_days_active(0, AT(0), 0) == 0,  "a null state is survivable here too");
    }

    /* ------------------------------------------------------------- folding a log */
    {
        GuruAgg a;
        guru_agg_reset(&a);
        CK(a.n == 0,                      "a reset fold is empty");
        CK(guru_top_cat(&a) == -1,        "an empty fold has no top category");
        CK(guru_weakest_cat(&a) == -1,    "and no weakest one");

        GuruRec r = { 0, 1, GU_CAT_GUT, 0 };
        guru_agg_add(&a, &r);
        guru_agg_add(&a, &r);
        r.cat = GU_CAT_METAB;
        guru_agg_add(&a, &r);
        CK(a.n == 3,                          "three checks fold to three");
        CK(a.cat[GU_CAT_GUT] == 2,            "and land in their categories");
        CK(guru_top_cat(&a) == GU_CAT_GUT,    "the busiest category is the top one");

        /* an undo is a row in the log, not an erasure -- the fold nets it out */
        r.cat = GU_CAT_GUT; r.flags = GU_F_UNDO;
        guru_agg_add(&a, &r);
        CK(a.n == 2,                          "an undo record nets out of the total");
        CK(a.cat[GU_CAT_GUT] == 1,            "and out of its category");

        /* an undo with no matching check in range must not wrap a uint16 */
        GuruAgg z;
        guru_agg_reset(&z);
        guru_agg_add(&z, &r);
        CK(z.n == 0,                          "an orphan undo clamps at zero");
        CK(z.cat[GU_CAT_GUT] == 0,            "rather than wrapping into a huge week");

        /* a record from a future category (a log written by a later build) is
         * ignored rather than indexed off the end of cat[] */
        GuruRec bad = { 0, 1, GU_NCAT, 0 };
        guru_agg_reset(&z);
        guru_agg_add(&z, &bad);
        CK(z.n == 0,                          "an unknown category is ignored, not indexed");
        guru_agg_add(0, &r);
        guru_agg_reset(0);
        CK(1,                                 "a null fold is survivable");
    }

    /* ------------------------------------------------------------- advice rules */
    {
        GuruAgg a;
        /* helper: fill a fold with per-category counts */
        #define FOLD(...) do{ int cc[GU_NCAT] = { __VA_ARGS__ }; guru_agg_reset(&a); \
                              for(int c = 0; c < GU_NCAT; c++){ GuruRec q = {0,1,(uint8_t)c,0}; \
                                  for(int i = 0; i < cc[c]; i++) guru_agg_add(&a, &q); } }while(0)

        /* R0 -- too little to say anything */
        FOLD(1,1,1,0,0);
        CK(guru_advise(&a, 3, 7) == GA_NONE,      "R0: four checks is not a week to analyse");
        guru_agg_reset(&a);
        CK(guru_advise(&a, 0, 7) == GA_NONE,      "R0: an empty week says nothing");
        CK(guru_advise(0, 3, 7) == GA_NONE,       "R0: a null fold says nothing");

        /* R1 -- one category got nothing at all */
        FOLD(3,3,2,2,0);
        CK(guru_advise(&a, 5, 7) == GA_NEGLECTED, "R1: an empty category is named");
        CK(guru_weakest_cat(&a) == GU_CAT_RECOV,  "R1: and it is the empty one");
        /* ...but not before there is enough of a week for the gap to mean something */
        FOLD(2,2,1,1,0);
        CK(guru_advise(&a, 4, 7) != GA_NEGLECTED, "R1: six checks is too early to call it neglected");

        /* R2 -- most of the week in one place */
        FOLD(1,7,1,1,1);
        CK(guru_advise(&a, 5, 7) == GA_NARROW,    "R2: a lopsided week is narrow");
        CK(guru_top_cat(&a) == GU_CAT_METAB,      "R2: and the crowded category is named");
        FOLD(2,3,2,2,2);
        CK(guru_advise(&a, 5, 7) != GA_NARROW,    "R2: an even week is not narrow");

        /* R3 -- volume, but crammed into a couple of days */
        FOLD(3,3,2,2,2);
        CK(guru_advise(&a, 3, 7) == GA_SPOTTY,    "R3: three days of a week is spotty");
        CK(guru_advise(&a, 2, 7) == GA_SPOTTY,    "R3: two is spottier still");

        /* R4 -- showed up almost every day */
        CK(guru_advise(&a, 7, 7) == GA_STEADY,    "R4: every day is steady");
        CK(guru_advise(&a, 6, 7) == GA_STEADY,    "R4: six of seven is still steady");

        /* R5 -- the unremarkable middle */
        CK(guru_advise(&a, 5, 7) == GA_KEEPGOING, "R5: five of seven is just keep going");
        CK(guru_advise(&a, 4, 7) == GA_KEEPGOING, "R5: and so is four");
        #undef FOLD
    }

    /* ------------------------------- R4: the week screens' day window (daycal.h) */
    {
        /* T0 is a Monday. "Now" is Sunday 09:00 UTC, six days on. */
        uint32_t now = AT(6);
        CK(cal_window_slot(now, now, 0, 7) == 6,             "today is the last slot");
        CK(cal_window_slot(AT(0), now, 0, 7) == 0,           "six days ago is the first");
        CK(cal_window_slot(AT(0) - DAY, now, 0, 7) == -1,    "a week ago is outside");
        CK(cal_window_slot(now + DAY, now, 0, 7) == -1,      "tomorrow is outside");
        CK(cal_window_slot(AT(0) - DAY, now, 0, 14) == 6,    "and slot 6 of a fortnight");
        /* a local-day window, not 168 hours: 00:30 on the first day is IN it */
        CK(cal_window_slot(T0 + 1800u, now, 0, 7) == 0,      "the whole first day counts");
        CK(cal_window_start(now, 0, 7) == T0,                "the window starts at its midnight");
        /* EDT: 02:00 UTC Monday is still Sunday evening locally */
        CK(cal_window_slot(T0 + 2 * HOUR, AT(1), -240, 7) == 4, "EDT files 22:00 Sunday under Sunday (Tuesday is 6)");
        CK(cal_window_start(AT(6), -240, 7) == T0 + 4 * HOUR, "an EDT window starts at 04:00 UTC");
        CK(cal_weekday(cal_day_index(T0, 0)) == 1,            "T0 is a Monday");
        CK(cal_weekday(cal_day_index(now, 0)) == 0,           "and six days on is a Sunday");
        CK(cal_weekday(0) == 4 && cal_weekday(-1) == 3,       "1970-01-01 was a Thursday, before it a Wednesday");
    }

    printf(fails ? "== guru: %d FAILURE(S) ==\n" : "== guru: all passed ==\n", fails);
    return fails ? 1 : 0;
}
