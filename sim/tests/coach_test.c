/* coach_test.c -- host gate for the pure Coach logic (firmware/main/coach.c).
 *
 * Coach's advice is the part a user is asked to trust ("shift sessions earlier"),
 * so every rule branch is pinned here before a single pixel is drawn. Everything in
 * coach.c takes its clock and zone as arguments, so this runs identically on any
 * host in any locale, with no sleeping and no /sdcard.
 *
 * Covers: local-day/slot bucketing across negative zones and midnight, the outcome
 * byte packing, aggregate folding, all six advice rules (including the near-misses
 * that must NOT fire), streak arithmetic, and sigil normalisation. */
#include <stdio.h>
#include <string.h>
#include "coach.h"

static int fails = 0;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } else printf("  ok: %s\n",(m)); }while(0)

/* 2026-08-17 00:00:00 UTC, a Monday -- the anchor for every dated case below. */
#define T0 1786924800u
#define HOUR 3600u
#define DAY  86400u

/* build one record: `hour` is hours past T0 (i.e. UTC hour-of-day on day 0). */
static CoachRec mk(unsigned hour, int dom, int en, int res, int blk,
                   int planned, int actual){
    CoachRec r;
    r.start       = T0 + hour * HOUR;
    r.planned_min = (uint16_t)planned;
    r.actual_min  = (uint16_t)actual;
    r.domain      = (uint8_t)dom;
    r.energy      = (uint8_t)en;
    r.intent      = CO_INT_FINISH;
    r.outcome     = co_pack(res, blk);
    return r;
}

static void feed(CoachAgg *a, const CoachRec *recs, int n, int tz){
    coach_agg_reset(a);
    for(int i = 0; i < n; i++) coach_agg_add(a, &recs[i], tz);
}

int main(void){
    printf("== coach ==\n");

    /* ---------------------------------------------------------- time bucketing */
    CK(coach_local_hour(T0 + 9 * HOUR, 0) == 9,     "UTC hour reads straight through");
    CK(coach_slot(T0 + 9 * HOUR, 0)  == CO_MORNING,   "09:00 is a morning");
    CK(coach_slot(T0 + 12 * HOUR, 0) == CO_AFTERNOON, "12:00 is an afternoon");
    CK(coach_slot(T0 + 16 * HOUR, 0) == CO_AFTERNOON, "16:00 is an afternoon");
    CK(coach_slot(T0 + 17 * HOUR, 0) == CO_EVENING,   "17:00 is an evening");
    CK(coach_slot(T0 + 23 * HOUR, 0) == CO_EVENING,   "23:00 is an evening");

    /* a negative zone (the Americas) must floor into the PREVIOUS local day, not
     * truncate toward zero -- the bug that would file a 20:00 EDT session under
     * tomorrow. -240 = EDT. */
    CK(coach_local_hour(T0 + 2 * HOUR, -240) == 22,     "02:00 UTC is 22:00 EDT");
    CK(coach_day_index(T0 + 2 * HOUR, -240) ==
       coach_day_index(T0, 0) - 1,                      "02:00 UTC files under the previous EDT day");
    CK(coach_slot(T0 + 2 * HOUR, -240) == CO_EVENING,   "02:00 UTC is an EDT evening");
    /* and a positive zone rolls the other way */
    CK(coach_day_index(T0 + 23 * HOUR, 600) ==
       coach_day_index(T0, 0) + 1,                      "23:00 UTC files under the next AEST day");
    CK(coach_day_index(T0, 0) == coach_day_index(T0 + 23 * HOUR, 0),
                                                        "one UTC day is one day index");

    /* ------------------------------------------------------------ byte packing */
    {
        CoachRec r = mk(9, CO_DOM_CAREER, CO_ENERGY_HIGH,
                        CO_RES_STRUGGLED, CO_BLK_DISTRACT, 25, 25);
        CK(co_result(&r)  == CO_RES_STRUGGLED, "packed result round-trips");
        CK(co_blocker(&r) == CO_BLK_DISTRACT,  "packed blocker round-trips");
        CK(sizeof(CoachRec) == 12,             "CoachRec is 12 bytes");
        CK(sizeof(CoachSigil) == 80,           "CoachSigil is 80 bytes");
        /* every result/blocker pair must survive the nibble packing */
        int bad = 0;
        for(int res = 0; res < CO_NRES; res++)
            for(int blk = 0; blk < CO_NBLK; blk++){
                CoachRec q; q.outcome = co_pack(res, blk);
                if(co_result(&q) != res || co_blocker(&q) != blk) bad++;
            }
        CK(bad == 0, "all result x blocker pairs round-trip");
    }

    /* ------------------------------------------------------------- aggregation */
    {
        CoachRec recs[] = {
            mk(9,  CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_GREAT,     CO_BLK_NONE,     25, 25),
            mk(10, CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_OKAY,      CO_BLK_NONE,     25, 25),
            mk(14, CO_DOM_LEARN,  CO_ENERGY_LOW,  CO_RES_STRUGGLED, CO_BLK_TIRED,    25, 12),
            mk(15, CO_DOM_LEARN,  CO_ENERGY_LOW,  CO_RES_ABANDONED, CO_BLK_DISTRACT, 25,  5),
        };
        CoachAgg a; feed(&a, recs, 4, 0);
        CK(a.n == 4,                          "counts every record");
        CK(a.focus_min == 25 + 25 + 12 + 5,   "sums actual focus minutes");
        CK(a.dom[CO_DOM_CAREER] == 2,         "folds domain counts");
        CK(a.slot_n[CO_MORNING] == 2,         "buckets mornings");
        CK(a.slot_n[CO_AFTERNOON] == 2,       "buckets afternoons");
        CK(a.slot_bad[CO_MORNING] == 0,       "no bad mornings");
        CK(a.slot_bad[CO_AFTERNOON] == 2,     "both afternoons went badly");
        CK(a.en_bad[CO_ENERGY_LOW] == 2,      "low-energy sessions counted as bad");
        CK(a.en_great[CO_ENERGY_HIGH] == 1,   "high-energy Great counted");
        CK(coach_bad_pct(&a) == 50,           "bad rate is 50%");
        CK(coach_top_blocker(&a) == CO_BLK_TIRED ||
           coach_top_blocker(&a) == CO_BLK_DISTRACT, "top blocker is one of the two cited");
        CK(coach_energy_great_pct(&a, CO_ENERGY_HIGH) == 50, "high energy is 50% Great");
        CK(coach_energy_great_pct(&a, CO_ENERGY_MED) == -1,  "no data reads -1, not 0");
        CK(a.planned_lo == 25 && a.planned_hi == 25,         "length range is flat");
        /* The domain list grew (Family, Relationships) after the first release.
         * `domain` is a whole byte on disk, so the new values must fold like any
         * other and every value must still name itself -- an unnamed domain would
         * print as an empty column in the weekly report rather than fail loudly. */
        CoachRec grown[] = {
            mk(9,  CO_DOM_FAMILY, CO_ENERGY_MED, CO_RES_GREAT, CO_BLK_NONE, 25, 25),
            mk(10, CO_DOM_PEOPLE, CO_ENERGY_MED, CO_RES_GREAT, CO_BLK_NONE, 25, 25),
            mk(11, CO_DOM_PEOPLE, CO_ENERGY_MED, CO_RES_OKAY,  CO_BLK_NONE, 25, 25),
        };
        CoachAgg g; feed(&g, grown, 3, 0);
        CK(g.n == 3,                          "the added domains are not rejected");
        CK(g.dom[CO_DOM_FAMILY] == 1,         "folds Family");
        CK(g.dom[CO_DOM_PEOPLE] == 2,         "folds Relationships");
        CK(coach_top_domain(&g) == CO_DOM_PEOPLE, "top domain can be a new one");
        for(int d = 0; d < CO_NDOM; d++)
            CK(coach_domain_name(d)[0] != 0,  "every domain index has a name");
        CK(coach_domain_name(CO_NDOM)[0] == 0, "one past the end names nothing");

        /* a corrupt record (out-of-range enum from a truncated file) is dropped,
         * not folded in as garbage */
        CoachRec junk = mk(9, 99, 99, 99, 99, 25, 25);
        coach_agg_add(&a, &junk, 0);
        CK(a.n == 4,                          "out-of-range record is ignored");
    }

    /* -------------------------------------------------------------- R0: silence */
    {
        CoachRec recs[4];
        for(int i = 0; i < 4; i++)
            recs[i] = mk(9, CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_STRUGGLED,
                         CO_BLK_TIRED, 25, 5);
        CoachAgg a; feed(&a, recs, 4, 0);
        CK(coach_advise(&a) == CA_NONE, "R0: four dreadful sessions still say nothing");
        CoachAgg empty; coach_agg_reset(&empty);
        CK(coach_advise(&empty) == CA_NONE, "R0: an empty range says nothing");
    }

    /* ------------------------------------------------------------- R1: earlier */
    {
        /* three clean mornings, three failing afternoons: 0% bad vs 100% bad */
        CoachRec recs[] = {
            mk(9,  CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_GREAT,     CO_BLK_NONE,  25, 25),
            mk(10, CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_GREAT,     CO_BLK_NONE,  25, 25),
            mk(11, CO_DOM_CAREER, CO_ENERGY_MED,  CO_RES_OKAY,      CO_BLK_NONE,  25, 25),
            mk(14, CO_DOM_CAREER, CO_ENERGY_MED,  CO_RES_STRUGGLED, CO_BLK_KIDS,  25, 24),
            mk(15, CO_DOM_CAREER, CO_ENERGY_MED,  CO_RES_STRUGGLED, CO_BLK_KIDS,  25, 24),
            mk(16, CO_DOM_CAREER, CO_ENERGY_MED,  CO_RES_STRUGGLED, CO_BLK_KIDS,  25, 24),
        };
        CoachAgg a; feed(&a, recs, 6, 0);
        CK(coach_best_slot(&a)  == CO_MORNING,   "best slot is the morning");
        CK(coach_worst_slot(&a) == CO_AFTERNOON, "worst slot is the afternoon");
        CK(coach_slot_ok_pct(&a, CO_MORNING) == 100, "mornings are 100% ok");
        CK(coach_slot_ok_pct(&a, CO_AFTERNOON) == 0, "afternoons are 0% ok");
        CK(coach_advise(&a) == CA_EARLIER, "R1: later bucket fails 2x -> shift earlier");

        /* near-miss: the SAME split but reversed in time -- bad mornings, good
         * evenings -- must NOT tell someone to move earlier. */
        CoachRec rev[] = {
            mk(9,  CO_DOM_CAREER, CO_ENERGY_MED, CO_RES_STRUGGLED, CO_BLK_KIDS, 25, 24),
            mk(10, CO_DOM_CAREER, CO_ENERGY_MED, CO_RES_STRUGGLED, CO_BLK_KIDS, 25, 24),
            mk(11, CO_DOM_CAREER, CO_ENERGY_MED, CO_RES_STRUGGLED, CO_BLK_KIDS, 25, 24),
            mk(18, CO_DOM_CAREER, CO_ENERGY_MED, CO_RES_GREAT,     CO_BLK_NONE, 25, 25),
            mk(19, CO_DOM_CAREER, CO_ENERGY_MED, CO_RES_GREAT,     CO_BLK_NONE, 25, 25),
            mk(20, CO_DOM_CAREER, CO_ENERGY_MED, CO_RES_OKAY,      CO_BLK_NONE, 25, 25),
        };
        CoachAgg b; feed(&b, rev, 6, 0);
        CK(coach_advise(&b) != CA_EARLIER, "R1: bad mornings do NOT say shift earlier");

        /* near-miss: only two afternoons -- below CO_MIN_SLOT, so no comparison */
        CoachRec thin[] = {
            mk(9,  CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_GREAT,     CO_BLK_NONE, 25, 25),
            mk(10, CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_GREAT,     CO_BLK_NONE, 25, 25),
            mk(11, CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_OKAY,      CO_BLK_NONE, 25, 25),
            mk(14, CO_DOM_CAREER, CO_ENERGY_MED,  CO_RES_STRUGGLED, CO_BLK_KIDS, 25, 25),
            mk(15, CO_DOM_CAREER, CO_ENERGY_MED,  CO_RES_STRUGGLED, CO_BLK_KIDS, 25, 25),
        };
        CoachAgg c; feed(&c, thin, 5, 0);
        CK(coach_advise(&c) != CA_EARLIER, "R1: a two-session bucket is not enough to judge");
    }

    /* ------------------------------------------------------------- R2: shorter */
    {
        /* struggling AND cutting short: 5 of 6 bad, actual/planned = 60/150 = 40% */
        CoachRec recs[6];
        for(int i = 0; i < 5; i++)
            recs[i] = mk(9 + i, CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_STRUGGLED,
                         CO_BLK_WORK, 25, 10);
        recs[5] = mk(14, CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_GREAT, CO_BLK_NONE, 25, 10);
        CoachAgg a; feed(&a, recs, 6, 0);
        CK(coach_advise(&a) == CA_SHORTER, "R2: struggling + bailing early -> shorter");

        /* near-miss: struggling but running the FULL session -- length isn't the
         * problem, so R2 must not fire. */
        CoachRec full[6];
        for(int i = 0; i < 5; i++)
            full[i] = mk(9 + i, CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_STRUGGLED,
                         CO_BLK_WORK, 25, 25);
        full[5] = mk(14, CO_DOM_CAREER, CO_ENERGY_HIGH, CO_RES_GREAT, CO_BLK_NONE, 25, 25);
        CoachAgg b; feed(&b, full, 6, 0);
        CK(coach_advise(&b) != CA_SHORTER, "R2: full-length struggles are not a length problem");
    }

    /* -------------------------------------------------------------- R3: reduce */
    {
        /* low energy is 4 of 6 (>= 40%) and 3 of those 4 go badly (> 50%). Spread
         * across slots so R1 cannot claim it first. */
        CoachRec recs[] = {
            mk(9,  CO_DOM_HEALTH, CO_ENERGY_LOW,  CO_RES_STRUGGLED, CO_BLK_TIRED, 25, 25),
            mk(14, CO_DOM_HEALTH, CO_ENERGY_LOW,  CO_RES_STRUGGLED, CO_BLK_TIRED, 25, 25),
            mk(18, CO_DOM_HEALTH, CO_ENERGY_LOW,  CO_RES_STRUGGLED, CO_BLK_TIRED, 25, 25),
            mk(10, CO_DOM_HEALTH, CO_ENERGY_LOW,  CO_RES_OKAY,      CO_BLK_NONE,  25, 25),
            mk(15, CO_DOM_HEALTH, CO_ENERGY_HIGH, CO_RES_GREAT,     CO_BLK_NONE,  25, 25),
            mk(19, CO_DOM_HEALTH, CO_ENERGY_HIGH, CO_RES_GREAT,     CO_BLK_NONE,  25, 25),
        };
        CoachAgg a; feed(&a, recs, 6, 0);
        CK(coach_advise(&a) == CA_REDUCE, "R3: depleted and failing -> reduce intensity");

        /* near-miss: the same low-energy share, but those sessions go FINE */
        CoachRec ok[] = {
            mk(9,  CO_DOM_HEALTH, CO_ENERGY_LOW,  CO_RES_OKAY,  CO_BLK_NONE, 25, 25),
            mk(14, CO_DOM_HEALTH, CO_ENERGY_LOW,  CO_RES_OKAY,  CO_BLK_NONE, 25, 25),
            mk(18, CO_DOM_HEALTH, CO_ENERGY_LOW,  CO_RES_OKAY,  CO_BLK_NONE, 25, 25),
            mk(10, CO_DOM_HEALTH, CO_ENERGY_LOW,  CO_RES_OKAY,  CO_BLK_NONE, 25, 25),
            mk(15, CO_DOM_HEALTH, CO_ENERGY_HIGH, CO_RES_OKAY,  CO_BLK_NONE, 25, 25),
            mk(19, CO_DOM_HEALTH, CO_ENERGY_HIGH, CO_RES_OKAY,  CO_BLK_NONE, 25, 25),
        };
        CoachAgg b; feed(&b, ok, 6, 0);
        CK(coach_advise(&b) == CA_STEADY, "R3: low energy that works is left alone");
    }

    /* ------------------------------------------------------------ R4: increase */
    {
        /* 5 of 6 Great, every session run to completion, one fixed length */
        CoachRec recs[6];
        for(int i = 0; i < 5; i++)
            recs[i] = mk(9 + i, CO_DOM_CREATE, CO_ENERGY_HIGH, CO_RES_GREAT,
                         CO_BLK_NONE, 25, 25);
        recs[5] = mk(15, CO_DOM_CREATE, CO_ENERGY_HIGH, CO_RES_OKAY, CO_BLK_NONE, 25, 25);
        CoachAgg a; feed(&a, recs, 6, 0);
        CK(coach_advise(&a) == CA_INCREASE, "R4: cruising at a fixed length -> increase");

        /* near-miss: same results, but the length has already been changed once --
         * don't tell someone to raise a bar they are already moving. */
        CoachRec moved[6];
        for(int i = 0; i < 5; i++)
            moved[i] = mk(9 + i, CO_DOM_CREATE, CO_ENERGY_HIGH, CO_RES_GREAT,
                          CO_BLK_NONE, 25, 25);
        moved[5] = mk(15, CO_DOM_CREATE, CO_ENERGY_HIGH, CO_RES_GREAT, CO_BLK_NONE, 45, 45);
        CoachAgg b; feed(&b, moved, 6, 0);
        CK(coach_advise(&b) == CA_STEADY, "R4: a length already in motion is left alone");
    }

    /* --------------------------------------------------------------- R5: steady */
    {
        CoachRec recs[6];
        for(int i = 0; i < 6; i++)
            recs[i] = mk(9 + i, CO_DOM_CAREER, CO_ENERGY_MED, CO_RES_OKAY,
                         CO_BLK_NONE, 25, 25);
        CoachAgg a; feed(&a, recs, 6, 0);
        CK(coach_advise(&a) == CA_STEADY, "R5: a solid unremarkable week says same again");
    }

    /* --------------------------------------------------------------- the streak */
    {
        CoachState s; coach_state_init(&s);
        CK(s.pref_min == 25 && s.day_goal == 6, "first run defaults to 25 min / goal 6");
        CK(coach_streak_now(&s, T0, 0) == 0,    "no sessions means no streak");

        coach_note_completion(&s, T0 + 9 * HOUR, 0);
        CK(s.streak == 1 && s.today_n == 1 && s.total_n == 1, "first session starts a streak");

        coach_note_completion(&s, T0 + 11 * HOUR, 0);
        CK(s.streak == 1 && s.today_n == 2, "a second session the same day does not extend it");

        coach_note_completion(&s, T0 + DAY + 9 * HOUR, 0);
        CK(s.streak == 2 && s.today_n == 1, "the next day extends the streak and resets today");

        coach_note_completion(&s, T0 + 3 * DAY + 9 * HOUR, 0);
        CK(s.streak == 1, "skipping a day breaks the streak");
        CK(s.best_streak == 2, "best streak remembers the high-water mark");

        /* the stored streak decays on its own once the days pass */
        uint32_t last = T0 + 3 * DAY + 9 * HOUR;
        CK(coach_streak_now(&s, last, 0) == 1,             "streak is live on the same day");
        CK(coach_streak_now(&s, last + DAY, 0) == 1,       "streak survives until tomorrow ends");
        CK(coach_streak_now(&s, last + 2 * DAY, 0) == 0,   "streak is dead two days later");
        CK(coach_today_now(&s, last, 0) == 1,              "today count is live on the day");
        CK(coach_today_now(&s, last + DAY, 0) == 0,        "today count resets tomorrow");

        /* a backwards clock (no RTC: NTP corrects the epoch mid-week) must not
         * inflate the streak -- it resets rather than counting a negative gap. */
        CoachState b; coach_state_init(&b);
        coach_note_completion(&b, T0 + 5 * DAY, 0);
        coach_note_completion(&b, T0 + 1 * DAY, 0);
        CK(b.streak == 1, "a backwards clock resets the streak instead of extending it");
    }

    /* ---------------------------------------------------------------- the sigil */
    {
        CoachSigil sg;
        /* a plain diagonal: normalises to the full 0..100 box, endpoints intact */
        int16_t diag[] = { 10,10,  20,20,  30,30 };
        CK(coach_sigil_from_raw(&sg, diag, 3) == 3, "keeps a short stroke whole");
        CK(coach_sigil_ok(&sg),                     "a real stroke is drawable");
        CK(sg.xy[0] == 0   && sg.xy[1] == 0,        "first point maps to the box origin");
        CK(sg.xy[4] == 100 && sg.xy[5] == 100,      "last point maps to the box corner");

        /* a WIDE stroke keeps its aspect: x spans the box, y is centred and short */
        int16_t wide[] = { 0,0,  100,0,  200,10 };
        coach_sigil_from_raw(&sg, wide, 3);
        CK(sg.xy[0] == 0 && sg.xy[4] == 100,        "wide stroke spans x fully");
        CK(sg.xy[1] > 40 && sg.xy[1] < 60,          "wide stroke is centred vertically");

        /* a tap has no extent -- not a mark, and the caller must be told so */
        int16_t tap[] = { 50,50,  50,50 };
        CK(coach_sigil_from_raw(&sg, tap, 2) == 0,  "a zero-extent tap is not a sigil");
        CK(!coach_sigil_ok(&sg),                    "a rejected sigil is not drawable");
        CK(coach_sigil_from_raw(&sg, diag, 1) == 0, "a single point is not a sigil");
        CK(coach_sigil_from_raw(&sg, 0, 5) == 0,    "a null stroke is not a sigil");

        /* a long stroke is decimated to the slot but keeps both endpoints */
        static int16_t lng[2 * 200];
        for(int i = 0; i < 200; i++){ lng[2*i] = (int16_t)i; lng[2*i+1] = (int16_t)i; }
        int kept = coach_sigil_from_raw(&sg, lng, 200);
        CK(kept == CO_SIG_MAXPT,                    "a long stroke is decimated to the slot");
        CK(sg.xy[0] == 0 && sg.xy[1] == 0,          "decimation keeps the first point");
        CK(sg.xy[2*(kept-1)] == 100,                "decimation keeps the last point");
        /* every stored coordinate must be inside the box -- ui.c scales by it */
        int oob = 0;
        for(int i = 0; i < kept * 2; i++) if(sg.xy[i] < 0 || sg.xy[i] > 100) oob++;
        CK(oob == 0, "every normalised coordinate is inside the 0..100 box");
    }

    printf(fails ? "== coach: %d FAILURE(S) ==\n" : "== coach: all passed ==\n", fails);
    return fails ? 1 : 0;
}
