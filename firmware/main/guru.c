/* guru.c -- pure Guru logic. See guru.h for the contract and the reasoning behind
 * the rolling target.
 *
 * No LVGL, no ESP-IDF, no stdio: this file is the numbers, ui.c is the pixels and
 * the file I/O. Every wall-clock input is injected, so sim/tests/guru_test.c pins
 * every branch on the host in any locale. */
#include "guru.h"
#include "daycal.h"

/* ------------------------------------------------------------------ internals */
/* How many of the GU_WIN-1 days before `today` are real days -- days the user has
 * actually lived through with the app -- rather than an unused slot of the ring.
 * Days that elapsed since the state was last written count: they happened, and
 * they were empty, which is exactly the data the average wants. */
static int gu_prior_days(const GuruState *s, int32_t today){
    if(s->seen_days == 0) return 0;
    int32_t adv = today - s->last_day;
    if(adv < 0) adv = 0;                       /* a backwards clock adds no days */
    long p = (long)s->seen_days - 1 + adv;
    if(p > GU_WIN - 1) p = GU_WIN - 1;
    return (int)p;
}

/* The count stored for one local day, read WITHOUT rolling the state -- so it has
 * to reason about staleness itself. Three ways a day holds nothing: the state has
 * never been used, the day is after the one the state describes (time passed with
 * nothing checked off), or the day has fallen out of the ring. */
static int gu_count_on(const GuruState *s, int32_t day, int32_t today){
    if(s->seen_days == 0)         return 0;
    if(day > s->last_day)         return 0;
    if(day <= today - GU_WIN)     return 0;
    return s->day_n[cal_mod(day, GU_WIN)];
}

static int32_t gu_today(uint32_t now, int tz_off_min){
    return cal_day_index(now, tz_off_min);
}

/* ------------------------------------------------------------------ lifecycle */
void guru_state_init(GuruState *s){
    if(!s) return;
    unsigned char *p = (unsigned char *)s;
    for(unsigned i = 0; i < sizeof *s; i++) p[i] = 0;
    /* seen_days == 0 is the "never used" sentinel; the first roll seeds the rest. */
}

int guru_roll(GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    int32_t today = gu_today(now, tz_off_min);

    if(s->seen_days == 0){                     /* first ever use: start here */
        for(int i = 0; i < GU_WIN; i++) s->day_n[i] = 0;
        s->last_day    = today;
        s->last_active = today - 1;            /* no active day yet; see streak */
        s->seen_days   = 1;
        return 0;
    }

    int32_t adv = today - s->last_day;
    if(adv <= 0) return 0;                     /* current, or a backwards clock */

    /* zero the days passed over. Beyond a full window every slot is stale, so
     * clear the lot rather than walking a gap that could be years long. */
    if(adv >= GU_WIN){
        for(int i = 0; i < GU_WIN; i++) s->day_n[i] = 0;
    } else {
        for(int32_t d = s->last_day + 1; d <= today; d++) s->day_n[cal_mod(d, GU_WIN)] = 0;
    }

    s->last_day = today;
    long seen = (long)s->seen_days + adv;
    s->seen_days = (uint8_t)(seen > GU_WIN ? GU_WIN : seen);
    return (int)(adv > GU_WIN ? GU_WIN : adv);
}

/* ----------------------------------------------------------- checking things off */
int guru_note_check(GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    guru_roll(s, now, tz_off_min);
    int slot = cal_mod(s->last_day, GU_WIN);
    int was  = s->day_n[slot];

    if(was < GU_DAY_MAX) s->day_n[slot] = (uint8_t)(was + 1);
    if(s->total_n < 0xFFFFFFFFu) s->total_n++;

    if(was == 0){                              /* today becomes an active day */
        if(s->streak > 0 && s->last_active == s->last_day - 1){
            if(s->streak < 0xFFFF) s->streak++;
        } else {
            s->streak = 1;
        }
        s->last_active = s->last_day;
        if(s->streak > s->best_streak) s->best_streak = s->streak;
    }
    return s->day_n[slot];
}

int guru_note_uncheck(GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    guru_roll(s, now, tz_off_min);
    int slot = cal_mod(s->last_day, GU_WIN);
    if(s->day_n[slot] == 0) return 0;
    s->day_n[slot]--;
    if(s->total_n) s->total_n--;

    /* Taking back the last check un-makes the active day, so the streak has to
     * give back the day it was just granted -- otherwise a mis-tap and its undo
     * leave a day credited that has nothing on it. Yesterday is still in the ring,
     * so we can tell whether the streak survives at length-1 or ends entirely.
     * best_streak deliberately stands: it is a high-water mark, not a live count. */
    if(s->day_n[slot] == 0 && s->last_active == s->last_day){
        int32_t yday = s->last_day - 1;
        if(s->streak > 1 && gu_count_on(s, yday, s->last_day) > 0){
            s->streak--;
            s->last_active = yday;
        } else {
            s->streak      = 0;
            s->last_active = s->last_day - 1;
        }
    }
    return s->day_n[slot];
}

/* -------------------------------------------------------------------- read-outs */
int guru_today_n(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    int32_t today = gu_today(now, tz_off_min);
    return gu_count_on(s, today, today);
}

int guru_window_days(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    return gu_prior_days(s, gu_today(now, tz_off_min));
}

int guru_target(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return GU_MIN_TARGET;
    int32_t today = gu_today(now, tz_off_min);
    int p = gu_prior_days(s, today);
    if(p <= 0) return GU_MIN_TARGET;           /* nothing to average yet */

    long sum = 0;
    for(int d = 1; d <= p; d++) sum += gu_count_on(s, today - d, today);

    /* round half up: an average of 4.5 asks for 5, because the tie should lean
     * toward the direction the app is nudging. Integer throughout, so the host
     * test and the device agree exactly. */
    long t = (sum + p / 2) / p;
    return t < GU_MIN_TARGET ? GU_MIN_TARGET : (int)t;
}

int guru_met_today(const GuruState *s, uint32_t now, int tz_off_min){
    return guru_today_n(s, now, tz_off_min) >= guru_target(s, now, tz_off_min);
}

int guru_streak_now(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s || s->streak == 0) return 0;
    int32_t today = gu_today(now, tz_off_min);
    /* today or yesterday keeps it alive -- the day isn't over until it's over. */
    if(today == s->last_active || today == s->last_active + 1) return s->streak;
    return 0;
}

int guru_window_total(const GuruState *s, uint32_t now, int tz_off_min){
    if(!s) return 0;
    int32_t today = gu_today(now, tz_off_min);
    int sum = 0;
    for(int d = 0; d < GU_WIN; d++) sum += gu_count_on(s, today - d, today);
    return sum;
}
