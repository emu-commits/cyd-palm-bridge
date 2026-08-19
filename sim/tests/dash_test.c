/* dash_test.c -- host gate for the lock-screen dashboard's data rules (dash.c).
 *
 * The dashboard is a glance screen: it is read in a second, on the way past, and
 * whatever it says is believed without a second look. That makes a stale reading
 * worse than a missing one -- "58 degrees" from yesterday afternoon is not a late
 * number, it is the wrong number, and nothing on the screen argues with it.
 *
 * So the freshness cut is pinned here rather than left to the drawing code: what
 * counts as fresh, what a missing or corrupt snapshot does, and that the boundary
 * itself sits where the constant says it does.
 *
 * Also covers the age arithmetic the "synced N ago" line is built from, including
 * a snapshot stamped in the future (a device whose clock jumped backwards must not
 * report a negative age or wrap it into something enormous). */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dash.h"

static int fails = 0;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } else printf("  ok: %s\n",(m)); }while(0)

/* a valid snapshot generated `min` minutes ago */
static WxCache aged(int min){
    time_t now = 0; time(&now);
    WxCache w; memset(&w, 0, sizeof w);
    w.magic     = WX_MAGIC;
    w.gen_epoch = (int64_t)now - (int64_t)min * 60;
    w.cur_tempF = 74;
    w.nhours    = WX_HOURS;
    return w;
}

int main(void){
    printf("dash: age arithmetic\n");
    {
        WxCache w = aged(0);
        CK(dash_weather_age_min(&w) == 0,    "a snapshot made now is 0 minutes old");
        w = aged(90);
        CK(dash_weather_age_min(&w) == 90,   "90 minutes reads as 90");
        w = aged(-120);                       /* stamped two hours in the future */
        CK(dash_weather_age_min(&w) == 0,     "a future stamp clamps to 0, never negative");
        CK(dash_weather_age_min(NULL) == 0,   "NULL is 0, not a crash");
    }

    printf("dash: freshness gate\n");
    {
        WxCache w = aged(5);
        CK(dash_weather_fresh(&w, WX_STALE_MIN),        "minutes old is fresh");
        w = aged(WX_STALE_MIN - 1);
        CK(dash_weather_fresh(&w, WX_STALE_MIN),        "one minute inside the cut is fresh");
        w = aged(WX_STALE_MIN);
        CK(!dash_weather_fresh(&w, WX_STALE_MIN),       "exactly at the cut is NOT fresh");
        w = aged(WX_STALE_MIN + 1);
        CK(!dash_weather_fresh(&w, WX_STALE_MIN),       "past the cut is not fresh");
        w = aged(30 * 24 * 60);
        CK(!dash_weather_fresh(&w, WX_STALE_MIN),       "a month old is not fresh");
    }

    printf("dash: nothing to show\n");
    {
        WxCache w = aged(5);
        w.magic = 0;                          /* what a failed load leaves behind */
        CK(!dash_weather_fresh(&w, WX_STALE_MIN),  "no magic is never fresh, however recent");
        w.magic = WX_MAGIC + 1;
        CK(!dash_weather_fresh(&w, WX_STALE_MIN),  "a foreign magic is never fresh");
        CK(!dash_weather_fresh(NULL, WX_STALE_MIN),"NULL is never fresh");
    }

    printf("dash: the cut is a day\n");
    CK(WX_STALE_MIN == 24 * 60, "WX_STALE_MIN is 24 hours");

    printf(fails ? "dash: %d FAILED\n" : "dash: all passed\n", fails);
    return fails ? 1 : 0;
}
