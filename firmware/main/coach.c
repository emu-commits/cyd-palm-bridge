/* coach.c -- pure Coach logic. See coach.h for the contract and
 * docs/COACH_DESIGN.md for the product design.
 *
 * No LVGL, no ESP-IDF, no stdio: this file is the numbers, ui.c is the pixels and
 * the file I/O. Every wall-clock input is injected, so sim/tests/coach_test.c pins
 * every branch on the host in any locale. */
#include "coach.h"

/* a bucket needs this many sessions before its rate is worth comparing -- below it,
 * one bad afternoon reads as a 100% failure rate and the advice becomes noise. */
#define CO_MIN_SLOT 3
/* the rule engine stays silent until it has seen this many sessions (R0). */
#define CO_MIN_ADVISE 5

/* ---------------------------------------------------------------- time helpers */
/* Local seconds since the epoch. tz_off_min is minutes east of UTC, so a negative
 * offset (the Americas) can push a just-after-midnight UTC time into the previous
 * local day -- hence the floor division below rather than a plain divide, which
 * truncates toward zero and would put that moment on the wrong day. */
static int64_t co_local_secs(uint32_t epoch, int tz_off_min){
    return (int64_t)epoch + (int64_t)tz_off_min * 60;
}
static int64_t co_floordiv(int64_t a, int64_t b){
    int64_t q = a / b;
    if((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

int32_t coach_day_index(uint32_t epoch, int tz_off_min){
    return (int32_t)co_floordiv(co_local_secs(epoch, tz_off_min), 86400);
}

int coach_local_hour(uint32_t epoch, int tz_off_min){
    int64_t s = co_local_secs(epoch, tz_off_min);
    int64_t day = co_floordiv(s, 86400);
    int64_t rem = s - day * 86400;            /* always 0..86399 */
    return (int)(rem / 3600);
}

int coach_slot(uint32_t epoch, int tz_off_min){
    int h = coach_local_hour(epoch, tz_off_min);
    if(h < 12) return CO_MORNING;
    if(h < 17) return CO_AFTERNOON;
    return CO_EVENING;
}

const char *coach_slot_name(int slot){
    switch(slot){
        case CO_MORNING:   return "mornings";
        case CO_AFTERNOON: return "afternoons";
        case CO_EVENING:   return "evenings";
    }
    return "";
}
const char *coach_domain_name(int dom){
    switch(dom){
        case CO_DOM_CAREER: return "Career";
        case CO_DOM_HEALTH: return "Health";
        case CO_DOM_LEARN:  return "Learning";
        case CO_DOM_CREATE: return "Creative";
    }
    return "";
}
const char *coach_energy_name(int en){
    switch(en){
        case CO_ENERGY_LOW:  return "Low";
        case CO_ENERGY_MED:  return "Medium";
        case CO_ENERGY_HIGH: return "High";
    }
    return "";
}
const char *coach_intent_name(int in){
    switch(in){
        case CO_INT_FINISH:   return "Finish";
        case CO_INT_EXPLORE:  return "Explore";
        case CO_INT_MAINTAIN: return "Maintain";
    }
    return "";
}
const char *coach_blocker_name(int blk){
    switch(blk){
        case CO_BLK_TIRED:    return "Tired";
        case CO_BLK_KIDS:     return "Kids";
        case CO_BLK_WORK:     return "Work";
        case CO_BLK_DISTRACT: return "Distracted";
    }
    return "None";
}
const char *coach_result_name(int res){
    switch(res){
        case CO_RES_GREAT:     return "Great";
        case CO_RES_OKAY:      return "Okay";
        case CO_RES_STRUGGLED: return "Struggled";
        case CO_RES_ABANDONED: return "Gave up";
    }
    return "";
}

/* ---------------------------------------------------------------- aggregation */
void coach_agg_reset(CoachAgg *a){
    if(!a) return;
    unsigned char *p = (unsigned char *)a;
    for(unsigned i = 0; i < sizeof *a; i++) p[i] = 0;
    /* an empty range has no length range yet; planned_lo stays "unset" as 0 and is
     * seeded by the first record (see coach_agg_add). */
}

void coach_agg_add(CoachAgg *a, const CoachRec *r, int tz_off_min){
    if(!a || !r) return;
    int res = co_result(r);
    int blk = co_blocker(r);
    int dom = r->domain, en = r->energy;
    if(res >= CO_NRES || blk >= CO_NBLK || dom >= CO_NDOM || en >= CO_NENERGY) return;

    int slot = coach_slot(r->start, tz_off_min);
    int bad  = CO_IS_BAD(res);

    a->n++;
    a->focus_min += r->actual_min;
    a->dom[dom]++;
    a->res[res]++;
    a->blk[blk]++;
    a->slot_n[slot]++;
    if(bad) a->slot_bad[slot]++;
    a->en_n[en]++;
    if(res == CO_RES_GREAT) a->en_great[en]++;
    if(bad) a->en_bad[en]++;
    a->planned_sum += r->planned_min;
    a->actual_sum  += r->actual_min;
    if(a->planned_lo == 0 || r->planned_min < a->planned_lo) a->planned_lo = r->planned_min;
    if(r->planned_min > a->planned_hi) a->planned_hi = r->planned_min;
}

/* ------------------------------------------------------------ derived read-outs */
int coach_slot_ok_pct(const CoachAgg *a, int slot){
    if(!a || slot < 0 || slot >= CO_NSLOT || a->slot_n[slot] == 0) return -1;
    int good = a->slot_n[slot] - a->slot_bad[slot];
    return good * 100 / a->slot_n[slot];
}

int coach_best_slot(const CoachAgg *a){
    if(!a) return -1;
    int best = -1, bestpct = -1;
    for(int s = 0; s < CO_NSLOT; s++){
        if(a->slot_n[s] < CO_MIN_SLOT) continue;
        int pct = coach_slot_ok_pct(a, s);
        /* ties go to the earlier slot: an equally good morning beats an evening. */
        if(pct > bestpct){ bestpct = pct; best = s; }
    }
    return best;
}

int coach_worst_slot(const CoachAgg *a){
    if(!a) return -1;
    int worst = -1, worstpct = 101;
    for(int s = 0; s < CO_NSLOT; s++){
        if(a->slot_n[s] < CO_MIN_SLOT) continue;
        int pct = coach_slot_ok_pct(a, s);
        /* ties go to the LATER slot, so "shift earlier" names the latest offender. */
        if(pct <= worstpct){ worstpct = pct; worst = s; }
    }
    return worst;
}

int coach_top_blocker(const CoachAgg *a){
    if(!a) return CO_BLK_NONE;
    int best = CO_BLK_NONE, bestn = 0;
    for(int b = CO_BLK_NONE + 1; b < CO_NBLK; b++)
        if(a->blk[b] > bestn){ bestn = a->blk[b]; best = b; }
    return best;
}

int coach_top_domain(const CoachAgg *a){
    if(!a || a->n == 0) return -1;
    int best = 0, bestn = -1;
    for(int d = 0; d < CO_NDOM; d++)
        if((int)a->dom[d] > bestn){ bestn = a->dom[d]; best = d; }
    return bestn > 0 ? best : -1;
}

int coach_energy_great_pct(const CoachAgg *a, int energy){
    if(!a || energy < 0 || energy >= CO_NENERGY || a->en_n[energy] == 0) return -1;
    return a->en_great[energy] * 100 / a->en_n[energy];
}

int coach_bad_pct(const CoachAgg *a){
    if(!a || a->n == 0) return 0;
    int bad = a->res[CO_RES_STRUGGLED] + a->res[CO_RES_ABANDONED];
    return bad * 100 / a->n;
}

/* ---------------------------------------------------------------- rule engine */
/* Fixed priority, first match wins. Each rule carries its own minimum sample so a
 * thin week can't trip it; R0 is the global floor. Integer math throughout -- the
 * decision path has no floating point, so the host test and the device agree
 * exactly. */
int coach_advise(const CoachAgg *a){
    if(!a || a->n < CO_MIN_ADVISE) return CA_NONE;          /* R0 */

    /* R1 -- shift earlier. The worst-performing bucket struggles at least twice as
     * often as the best, and it falls later in the day. Both buckets need
     * CO_MIN_SLOT sessions, and the offender has to be failing at least a third of
     * the time before we tell someone to rearrange their day. */
    {
        int b = coach_best_slot(a), w = coach_worst_slot(a);
        if(b >= 0 && w >= 0 && w > b){
            int bbad = 100 - coach_slot_ok_pct(a, b);
            int wbad = 100 - coach_slot_ok_pct(a, w);
            if(wbad >= 34 && wbad >= 2 * bbad) return CA_EARLIER;
        }
    }

    /* R2 -- try shorter. Struggling AND cutting sessions short: the length is the
     * problem, not the day. actual/planned < 0.8 as integers. */
    if(coach_bad_pct(a) > 40 &&
       a->planned_sum > 0 && a->actual_sum * 100 < a->planned_sum * 80)
        return CA_SHORTER;

    /* R3 -- reduce intensity. Low-energy sessions are a large share of the range and
     * most of them go badly: grinding while depleted, and the data says so. */
    if(a->en_n[CO_ENERGY_LOW] * 100 >= a->n * 40 &&
       a->en_n[CO_ENERGY_LOW] > 0 &&
       a->en_bad[CO_ENERGY_LOW] * 100 > a->en_n[CO_ENERGY_LOW] * 50)
        return CA_REDUCE;

    /* R4 -- increase intensity. Mostly Great, running sessions to completion, and the
     * length has not moved across the whole range (planned_lo == planned_hi). */
    if(a->res[CO_RES_GREAT] * 100 > a->n * 70 &&
       a->planned_sum > 0 && a->actual_sum >= a->planned_sum &&
       a->planned_lo == a->planned_hi)
        return CA_INCREASE;

    return CA_STEADY;                                        /* R5 */
}

/* ------------------------------------------------------------ streak bookkeeping */
void coach_state_init(CoachState *s){
    if(!s) return;
    unsigned char *p = (unsigned char *)s;
    for(unsigned i = 0; i < sizeof *s; i++) p[i] = 0;
    s->phase    = CO_PH_IDLE;
    s->day_goal = 6;
    s->pref_min = 25;
    s->last_day = 0;
}

void coach_note_completion(CoachState *s, uint32_t when, int tz_off_min){
    if(!s) return;
    int32_t day = coach_day_index(when, tz_off_min);
    if(s->total_n == 0){                 /* first session ever */
        s->streak  = 1;
        s->today_n = 1;
    } else if(day == s->last_day){       /* same local day: streak unchanged */
        if(s->today_n < 0xFFFF) s->today_n++;
    } else if(day == s->last_day + 1){   /* consecutive day: extend */
        if(s->streak < 0xFFFF) s->streak++;
        s->today_n = 1;
    } else {                             /* a gap (or the clock went backwards) */
        s->streak  = 1;
        s->today_n = 1;
    }
    s->last_day = day;
    if(s->total_n < 0xFFFF) s->total_n++;
    if(s->streak > s->best_streak) s->best_streak = s->streak;
}

int coach_streak_now(const CoachState *s, uint32_t now, int tz_off_min){
    if(!s || s->total_n == 0) return 0;
    int32_t today = coach_day_index(now, tz_off_min);
    /* today or yesterday keeps it alive -- the day isn't over until it's over. */
    if(today == s->last_day || today == s->last_day + 1) return s->streak;
    return 0;
}

int coach_today_now(const CoachState *s, uint32_t now, int tz_off_min){
    if(!s || s->total_n == 0) return 0;
    return coach_day_index(now, tz_off_min) == s->last_day ? s->today_n : 0;
}

/* ---------------------------------------------------------------------- sigil */
int coach_sigil_ok(const CoachSigil *sg){ return sg && sg->n >= 2; }

int coach_sigil_from_raw(CoachSigil *sg, const int16_t *xy, int n){
    if(!sg) return 0;
    unsigned char *p = (unsigned char *)sg;
    for(unsigned i = 0; i < sizeof *sg; i++) p[i] = 0;
    if(!xy || n < 2) return 0;

    int minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
    for(int i = 1; i < n; i++){
        int x = xy[2*i], y = xy[2*i + 1];
        if(x < minx) minx = x;
        if(x > maxx) maxx = x;
        if(y < miny) miny = y;
        if(y > maxy) maxy = y;
    }
    int w = maxx - minx, h = maxy - miny;
    int span = w > h ? w : h;
    /* a tap (or a stroke with no extent) is not a mark -- the caller falls back to a
     * plain filled square rather than drawing a single dot nobody can tell apart. */
    if(span <= 0) return 0;

    /* keep aspect: scale both axes by the same span and centre the short one. */
    int offx = (span - w) / 2, offy = (span - h) / 2;

    /* decimate evenly if the stroke is longer than the slot. Endpoints are kept:
     * index i maps to raw index i*(n-1)/(keep-1), so the first and last raw points
     * always survive and the mark keeps its start and finish. */
    int keep = n > CO_SIG_MAXPT ? CO_SIG_MAXPT : n;
    for(int i = 0; i < keep; i++){
        int src = keep > 1 ? (int)((long)i * (n - 1) / (keep - 1)) : 0;
        int x = ((xy[2*src]     - minx) + offx) * 100 / span;
        int y = ((xy[2*src + 1] - miny) + offy) * 100 / span;
        if(x < 0) x = 0;
        if(x > 100) x = 100;
        if(y < 0) y = 0;
        if(y > 100) y = 100;
        sg->xy[2*i]     = (int8_t)x;
        sg->xy[2*i + 1] = (int8_t)y;
    }
    sg->n = (uint8_t)keep;
    return keep;
}
