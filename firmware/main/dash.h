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
#define WX_MAGIC  0x57583031u          /* "WX01" */
#define WX_HOURS  6                     /* hourly forecast columns kept for the strip */

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
    uint8_t  nhours;                    /* valid entries in hr[] (<= WX_HOURS) */
    struct {
        uint8_t hour24;                 /* hour of day 0..23 */
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
