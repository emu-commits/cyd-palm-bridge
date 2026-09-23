/* dash.h -- lock-screen dashboard data: weather cache + astronomy (see PRODUCT_PLAN.md).
 *
 * Pure C (no LVGL, no ESP-IDF) so it also builds in the simulator and host tests.
 * The dashboard renders entirely OFFLINE from these: the weather is a compact blob
 * refreshed on the SD card during a HotSync (device task, later), the moon phase and
 * (as a fallback) the sun times are computed on-device from the date. */
#ifndef DASH_H
#define DASH_H
#include <stdint.h>
#include <time.h>

#define WX_PATH   "/sdcard/weather.dat"
#define WX_MAGIC  0x57583032u          /* "WX02" -- WX01 had 6 hours and no per-hour code */
/* A DAY of hourly rows, not just the six the strip draws. The snapshot is
 * refreshed once per HotSync -- about once a day in practice -- so a cache that
 * only reached six hours forward left the dashboard showing the temperature it
 * was at sync time for the rest of the day. Twenty-four rows means the forecast
 * can be STEPPED THROUGH as the hours pass, with no network in between, which is
 * the whole premise of an offline glance screen. */
#define WX_HOURS  24                    /* hourly rows kept (the strip still draws 6) */
#define WX_STRIP  6                     /* columns the dashboard draws at once */

/* A tiny, fixed-size weather snapshot. Written by the sync task (or the sample
 * seeder) and read by the dashboard; same platform writes and reads it, so struct
 * layout need only be self-consistent. Temperatures are whole degrees Fahrenheit. */
typedef struct {
    uint32_t magic;
    int64_t  gen_epoch;                 /* unix time the snapshot was made (for "synced Nh ago") */
    int16_t  cur_tempF;                 /* current temperature, degrees F */
    uint8_t  cur_code;                  /* WMO weather-interpretation code */
    int16_t  aqi;                       /* US AQI, or -1 if unknown */
    int16_t  sunrise_min;               /* local minutes since midnight, or -1 */
    int16_t  sunset_min;                /* local minutes since midnight, or -1 */
    /* Unix time of hr[0]. The rows are consecutive hours from here, so "which row
     * covers now" is arithmetic and cannot be ambiguous. Matching on hour-of-day
     * CANNOT work at this size: twenty-four rows contain every hour of the day, so
     * a clock that has run a day and a half past the fetch would match a row from
     * the wrong day and read as current. */
    int64_t  hr0_epoch;
    uint8_t  nhours;                    /* valid entries in hr[] (<= WX_HOURS) */
    struct {
        uint8_t hour24;                 /* hour of day 0..23 */
        uint8_t code;                   /* WMO code for this hour (0 if the feed omitted it) */
        int16_t tempF;                  /* temperature, degrees F */
        uint8_t rain;                   /* precipitation probability 0..100 % */
    } hr[WX_HOURS];
} WxCache;

/* read WX_PATH into *out; 1 on success (valid magic), 0 otherwise. */
int  dash_weather_load(WxCache *out);

/* if `path` has no valid snapshot yet, write a plausible sample (so the dashboard
 * renders on the sim/first boot before a real fetch). Safe to call every boot. */
void dash_weather_seed_sample(const char *path);

/* whole minutes since the snapshot was generated (using time(NULL)); 0 if in future. */
int  dash_weather_age_min(const WxCache *w);

/* How old a snapshot may get before the dashboard stops drawing it. Past a day
 * "now", the six-hour strip and today's sun times are not merely late, they are
 * wrong, and a wrong number on a glance screen is worse than a missing one. */
#define WX_STALE_MIN (24 * 60)

/* 1 if *w is recent enough to show: a valid snapshot less than max_min old.
 * NULL, or anything that did not load, is never fresh. */
int  dash_weather_fresh(const WxCache *w, int max_min);

/* ---- stepping through the cached day -------------------------------------
 * Index of the hr[] row covering local time `t`, or -1 if the snapshot does not
 * reach that hour. Rows are consecutive hours starting from the fetch, so an
 * hour-of-day matches at most one row and a plain scan is unambiguous.
 *
 * Nothing here re-fetches or extrapolates: if the day has run past the end of
 * the cache the answer is -1, and the caller falls back to the fetch-time
 * reading (which at least announces its own age) rather than inventing one. */
int dash_wx_index_at(const WxCache *w, time_t t);

/* The reading to SHOW at time `t`: the hourly row covering now when the cache has
 * one, else the fetch-time `cur_*` fields. `tempF`/`code` may be NULL. Returns 1
 * if the answer came from the hourly rows (i.e. it has been stepped forward),
 * 0 if it is the fetch-time reading. */
int dash_wx_now(const WxCache *w, time_t t, int *tempF, int *code);

/* short label for a WMO weather code, e.g. 2 -> "Partly cloudy". Never NULL. */
const char *dash_wcode_desc(int code);

/* moon phase for time t: illumination 0..100, *waxing=1 while growing, and a name
 * ("New", "Waxing crescent", "First quarter", ... "Waning crescent"). */
void dash_moon(time_t t, int *illum_pct, int *waxing, const char **name);

/* sunrise/sunset for the given local date at (lat, lon east-positive), returned as
 * local minutes since midnight using tz_off_min (minutes east of UTC). Sets -1 for
 * polar day/night. A standalone fallback; the real forecast carries its own times. */
void dash_sun_times(int year, int mon, int day, double lat, double lon,
                    int tz_off_min, int *rise_min, int *set_min);

#endif
