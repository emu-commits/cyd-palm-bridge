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
#include <stdlib.h>
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

/* A snapshot holding a full day, hour `start` onward, with a temperature that is
 * distinct per row so an off-by-one in the stepping cannot pass. */
static WxCache dayfrom(int start, int64_t hr0){
    WxCache w; memset(&w, 0, sizeof w);
    w.magic = WX_MAGIC; w.nhours = WX_HOURS; w.hr0_epoch = hr0;
    w.cur_tempF = 50; w.cur_code = 95;          /* deliberately unlike any hourly row */
    for(int i = 0; i < WX_HOURS; i++){
        w.hr[i].hour24 = (uint8_t)((start + i) % 24);
        w.hr[i].tempF  = (int16_t)(60 + i);
        w.hr[i].rain   = (uint8_t)i;
        w.hr[i].code   = (uint8_t)(i == 0 ? 0 : 3);   /* row 0 has none -> must fall back */
    }
    return w;
}

int main(void){
    /* The stepping is pure epoch arithmetic and needs no zone, but the age
     * arithmetic below calls time(), so pin the zone for reproducibility. */
    setenv("TZ", "UTC", 1); tzset();

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

    printf("dash: stepping through the cached day\n");
    {
        /* 2026-08-27T09:00Z; the snapshot starts at 09:00 and runs 24 hours. */
        const time_t T9 = 1787821200;
        WxCache w = dayfrom(9, T9);
        int tf = 0, cd = 0;

        CK(dash_wx_index_at(&w, T9) == 0,          "the fetch hour is row 0");
        CK(dash_wx_index_at(&w, T9 + 3*3600) == 3, "three hours on is row 3");
        CK(dash_wx_index_at(&w, T9 + 23*3600) == 23, "the last cached hour is row 23");

        CK(dash_wx_now(&w, T9 + 5*3600, &tf, &cd) == 1, "a covered hour reports as stepped");
        CK(tf == 65,                                "and gives THAT hour's temperature");
        CK(cd == 3,                                 "and that hour's code");

        /* This is the bug the widening was for: six hours on, the reading must have
         * moved off the one taken at sync time. */
        dash_wx_now(&w, T9 + 6*3600, &tf, NULL);
        CK(tf == 66 && tf != w.cur_tempF,           "six hours on it is NOT the sync-time reading");

        /* A row the feed gave no code for keeps the fetch-time code rather than
         * reading as WMO 0 ("Clear"), which would be a fabricated forecast. */
        dash_wx_now(&w, T9, NULL, &cd);
        CK(cd == 95,                                "a row with no code falls back, not to 'Clear'");

        /* Walked off the end: say so, do not wrap around to row 0. */
        CK(dash_wx_index_at(&w, T9 + 25*3600) == -1, "past the cache is -1, not a wrap");
        CK(dash_wx_now(&w, T9 + 25*3600, &tf, &cd) == 0, "and reports as NOT stepped");
        CK(tf == 50 && cd == 95,                    "falling back to the fetch-time reading");

        CK(dash_wx_index_at(&w, T9 - 60) == -1,     "before the first row is -1");

        WxCache e; memset(&e, 0, sizeof e); e.magic = WX_MAGIC; e.nhours = 0;
        CK(dash_wx_index_at(&e, T9) == -1,          "an empty cache is -1");
        WxCache u = dayfrom(9, 0);                  /* no anchor -> refuse, do not guess */
        CK(dash_wx_index_at(&u, T9) == -1,          "an unanchored cache is -1, not row 0");
        CK(dash_wx_index_at(NULL, T9) == -1,        "NULL is -1, not a crash");
        tf = 7; cd = 7;
        CK(dash_wx_now(NULL, T9, &tf, &cd) == 0 && tf == 0, "NULL through dash_wx_now is safe");
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
