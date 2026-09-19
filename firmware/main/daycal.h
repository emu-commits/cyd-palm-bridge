/* daycal.h -- local-day arithmetic with an injected zone. Header-only, pure.
 *
 * "Which local day is this epoch on?" is asked by every app that counts days:
 * Coach streaks sessions, Guru streaks habits, and both bucket records by
 * time-of-day. The arithmetic is short but it has one sharp edge, so it lives in
 * one place rather than once per engine.
 *
 * The edge: C integer division truncates TOWARD ZERO, and a negative zone offset
 * (the Americas) can push a just-after-midnight UTC time to a NEGATIVE local
 * second count near the epoch -- and, more importantly, the same truncation files
 * a 20:00 EDT session under tomorrow. Floor division is the correct operation for
 * a calendar; a plain `/` is not. coach_test.c pins that case explicitly.
 *
 * `tz_off_min` is minutes east of UTC. It is passed in, never read from TZ, which
 * is what makes every engine above this host-testable in any locale.
 */
#ifndef DAYCAL_H
#define DAYCAL_H
#include <stdint.h>

static inline int64_t cal_local_secs(uint32_t epoch, int tz_off_min){
    return (int64_t)epoch + (int64_t)tz_off_min * 60;
}

static inline int64_t cal_floordiv(int64_t a, int64_t b){
    int64_t q = a / b;
    if((a % b) != 0 && ((a < 0) != (b < 0))) q--;
    return q;
}

/* local day number since the epoch -- the unit a streak counts in. */
static inline int32_t cal_day_index(uint32_t epoch, int tz_off_min){
    return (int32_t)cal_floordiv(cal_local_secs(epoch, tz_off_min), 86400);
}

/* local hour 0..23. */
static inline int cal_local_hour(uint32_t epoch, int tz_off_min){
    int64_t s   = cal_local_secs(epoch, tz_off_min);
    int64_t day = cal_floordiv(s, 86400);
    return (int)((s - day * 86400) / 3600);      /* the remainder is always 0..86399 */
}

/* a non-negative modulus, for indexing a ring by day number. C's % keeps the sign
 * of the dividend, so a negative day index would index backwards off the array. */
static inline int cal_mod(int32_t a, int m){
    int r = (int)(a % m);
    return r < 0 ? r + m : r;
}

#endif
