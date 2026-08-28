/* wx_test.c -- offline gate for the Open-Meteo CSV -> WxCache parse.
 *
 * The fixtures in tests/data/ are VERBATIM responses from the live API, so this
 * gate fails if Open-Meteo changes its CSV shape -- which is the whole point:
 * the device has no way to tell a reordered column from a plausible temperature.
 * No network needed.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../bridge/wxfetch.h"

static int failures;
#define CHECK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); failures++; } }while(0)

int main(void){
    printf("Open-Meteo CSV gate\n");

    /* --- URL building: the coordinate guard ------------------------------- */
    char u[512];
    CHECK(wx_build_url(u,sizeof u,"40.7143","-74.0060") > 0, "builds a URL from valid coords");
    CHECK(strstr(u,"latitude=40.7143") && strstr(u,"longitude=-74.0060"), "coords land in the URL");
    CHECK(strstr(u,"format=csv")   != NULL, "asks for CSV, not JSON");
    CHECK(strstr(u,"timezone=auto")!= NULL, "asks for local timestamps");
    CHECK(strstr(u,"fahrenheit")   != NULL, "asks for the unit the cache stores");
    CHECK(wx_build_url(u,sizeof u,"","-74.0")   == 0 && u[0]==0, "empty latitude is refused");
    CHECK(wx_build_url(u,sizeof u,"40.7","")    == 0, "empty longitude is refused");
    CHECK(wx_build_url(u,sizeof u,"N/A","-74.0")== 0, "non-numeric latitude is refused");
    CHECK(wx_build_url(u,sizeof u,"40.7.1","-74")==0, "two dots is refused");
    CHECK(wx_build_url(u,sizeof u,"40.7 # home","-74")==0,
          "a coordinate with a trailing comment never reaches the server");
    CHECK(wx_build_url(u,sizeof u,"-33.87","151.21") > 0, "southern/eastern coords are fine");
    CHECK(wx_build_url(u,32,"40.7143","-74.0060") == 0, "a too-small buffer refuses rather than truncates");
    CHECK(wx_build_aqi_url(u,sizeof u,"40.7","-74.0") > 0 && strstr(u,"us_aqi"), "AQI URL");
    CHECK(strstr(u,"air-quality") != NULL, "AQI uses the air-quality host");

    /* --- parsing a real response ------------------------------------------ */
    WxCache w;
    /* the fixture's "current" row is 2026-08-20T19:30 local; 1787268600 is that
     * instant in UTC-4, which is what the device clock would read. */
    const int64_t NOW = 1787268600LL;
    CHECK(wx_parse_file("data/openmeteo.csv", NOW, &w) == 1, "parses the live fixture");
    CHECK(w.magic == WX_MAGIC,      "magic set (dash_weather_load will accept it)");
    CHECK(w.gen_epoch == NOW,       "stamped with the fetch time, for the age line");
    CHECK(w.cur_tempF == 68,        "current temperature rounded from 68.4");
    CHECK(w.cur_code == 55,         "current WMO code");
    CHECK(w.nhours == WX_HOURS,     "a full day of hourly rows is kept, not six");
    CHECK(w.hr[0].hour24 == 20,     "the rows start at the NEXT hour, not midnight");
    CHECK(w.hr[1].hour24 == 21 && w.hr[2].hour24 == 22, "and run forward from there");
    /* The anchor is what makes the dashboard able to step: this fixture's first
     * kept row is 2026-08-20T20:00 at UTC-4. Matching on hour-of-day could not do
     * this job -- 24 rows contain every hour, so a day-old clock would match. */
    CHECK(w.hr0_epoch == 1787270400LL, "hr[0] is stamped with its own Unix time");
    CHECK(w.hr0_epoch % 3600 == 0,     "and it lands on the top of an hour");
    /* This fixture predates hourly weather_code; absence must read as absence. */
    CHECK(w.hr[0].code == 0,        "an hourly block without weather_code leaves it unset");
    for(int i=0;i<w.nhours;i++){
        CHECK(w.hr[i].hour24 <= 23,       "hour in range");
        CHECK(w.hr[i].rain <= 100,        "rain probability is a percentage");
        CHECK(w.hr[i].tempF > -80 && w.hr[i].tempF < 140, "temperature is plausible");
    }
    CHECK(w.sunrise_min == 6*60 + 11, "sunrise 06:11 as local minutes");
    CHECK(w.sunset_min  == 19*60 + 47,"sunset 19:47 as local minutes");
    CHECK(w.aqi == -1,                "AQI is absent from the forecast response");

    /* --- the response we actually ask for now, with hourly weather_code ------
     * Verbatim from the live API on 2026-08-27. The hourly block gained a
     * weather_code column, which is also the column the parser USED to tell the
     * current block from the hourly one -- so this fixture is the regression test
     * for that discriminator as much as for the new field. */
    {
        WxCache c;
        const int64_t NOW2 = 1787875200LL;   /* 2026-08-27T20:00 local, UTC-4 */
        CHECK(wx_parse_file("data/openmeteo_code.csv", NOW2, &c) == 1, "parses the hourly-code fixture");
        CHECK(c.cur_tempF == 72, "current temperature, not an hourly row");
        CHECK(c.cur_code  == 51, "current WMO code, not an hourly row");
        CHECK(c.nhours == WX_HOURS, "a full day of rows");
        CHECK(c.hr[0].hour24 == 21, "rows start at the hour after the current reading");
        CHECK(c.hr[0].tempF  == 72 && c.hr[0].rain == 28, "row 0 temperature and rain");
        CHECK(c.hr[0].code   == 53, "row 0 carries its OWN weather code");
        CHECK(c.hr[1].code   == 3,  "and row 1 a different one -- the code steps too");
        CHECK(c.hr0_epoch == NOW2 + 3600, "anchored one hour after the current reading");
        CHECK(c.sunrise_min == 6*60 + 18 && c.sunset_min == 19*60 + 36, "sun times");
        int stepped = 0, tf = 0, cd = 0;
        stepped = dash_wx_now(&c, (time_t)(NOW2 + 6*3600), &tf, &cd);
        CHECK(stepped == 1 && tf != c.cur_tempF,
              "six hours on, the dashboard reading has moved off the sync-time one");
    }

    /* --- the AQI response -------------------------------------------------- */
    int aqi = wx_parse_aqi_file("data/openmeteo_aqi.csv");
    CHECK(aqi >= 0 && aqi <= 1000, "AQI parsed and in range");

    /* --- failure modes ------------------------------------------------------ */
    WxCache keep = w;
    CHECK(wx_parse_file("data/does_not_exist.csv", NOW, &w) == 0, "missing file -> 0");
    CHECK(memcmp(&keep,&w,sizeof w) == 0, "a failed parse leaves the caller's cache untouched");
    CHECK(wx_parse_file("data/openmeteo_aqi.csv", NOW, &w) == 0,
          "the WRONG response body is rejected, not half-parsed");
    CHECK(wx_parse_aqi_file("data/openmeteo.csv") == -1, "and the same in reverse");

    printf(failures ? "\nWeather gate: %d FAIL\n" : "\nWeather gate: OK\n", failures);
    return failures ? 1 : 0;
}
