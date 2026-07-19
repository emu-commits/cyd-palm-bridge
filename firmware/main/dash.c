/* dash.c -- lock-screen dashboard data (see dash.h). Pure C + libm. */
#include "dash.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- weather cache */

int dash_weather_load(WxCache *out){
    if(!out) return 0;
    FILE *f = fopen(WX_PATH, "rb");
    if(!f) return 0;
    WxCache w;
    size_t n = fread(&w, 1, sizeof w, f);
    fclose(f);
    if(n != sizeof w || w.magic != WX_MAGIC) return 0;
    if(w.nhours > WX_HOURS) w.nhours = WX_HOURS;
    *out = w;
    return 1;
}

int dash_weather_age_min(const WxCache *w){
    if(!w) return 0;
    time_t now = 0; time(&now);
    long d = (long)((long long)now - (long long)w->gen_epoch);
    return d > 0 ? (int)(d / 60) : 0;
}

/* WMO weather-interpretation codes (Open-Meteo's `weathercode`), collapsed to the
 * few buckets a one-line label needs. */
const char *dash_wcode_desc(int code){
    switch(code){
        case 0:                 return "Clear";
        case 1:                 return "Mostly clear";
        case 2:                 return "Partly cloudy";
        case 3:                 return "Overcast";
        case 45: case 48:       return "Fog";
        case 51: case 53: case 55:
        case 56: case 57:       return "Drizzle";
        case 61: case 63:       return "Rain";
        case 65:                return "Heavy rain";
        case 66: case 67:       return "Freezing rain";
        case 71: case 73: case 75:
        case 77:                return "Snow";
        case 80: case 81:       return "Showers";
        case 82:                return "Heavy showers";
        case 85: case 86:       return "Snow showers";
        case 95:                return "Thunderstorm";
        case 96: case 99:       return "Thunder + hail";
        default:                return "--";
    }
}

/* Write a plausible sample snapshot if one isn't already present. Sun times are
 * computed for a sample location (New York); the real fetch replaces all of this. */
void dash_weather_seed_sample(const char *path){
    if(!path) path = WX_PATH;
    FILE *t = fopen(path, "rb");
    if(t){                                       /* already seeded / fetched? leave it */
        WxCache probe; size_t n = fread(&probe, 1, sizeof probe, t); fclose(t);
        if(n == sizeof probe && probe.magic == WX_MAGIC) return;
    }
    time_t now = 0; time(&now);
    struct tm lt; localtime_r(&now, &lt);

    WxCache w; memset(&w, 0, sizeof w);
    w.magic     = WX_MAGIC;
    w.gen_epoch = (int64_t)now - 2 * 3600;       /* pretend it was fetched 2h ago */
    w.cur_tempF = 74;
    w.cur_code  = 2;                             /* partly cloudy */
    w.aqi       = 41;
    /* fixed, plausible sample sun times (the real fetch fills these per location;
     * dash_sun_times() is the on-device fallback when a fetch omits them). */
    w.sunrise_min = 5 * 60 + 52;
    w.sunset_min  = 20 * 60 + 31;
    w.nhours    = WX_HOURS;
    /* a warm afternoon with rain building -- matches the approved mock-up */
    static const int16_t temps[WX_HOURS] = { 76, 79, 81, 82, 80, 77 };
    static const uint8_t rains[WX_HOURS] = { 10,  5,  0, 20, 55, 70 };
    for(int i = 0; i < WX_HOURS; i++){
        w.hr[i].hour24 = (uint8_t)((lt.tm_hour + 1 + i) % 24);
        w.hr[i].tempF  = temps[i];
        w.hr[i].rain   = rains[i];
    }
    FILE *f = fopen(path, "wb");
    if(!f) return;
    fwrite(&w, 1, sizeof w, f);
    fclose(f);
}

/* ------------------------------------------------------------------- moon phase */

void dash_moon(time_t t, int *illum_pct, int *waxing, const char **name){
    /* age within the synodic month, from a known new moon (2000-01-06 18:14 UTC). */
    double days = (double)t / 86400.0 + 2440587.5;      /* Julian day */
    const double SYN = 29.530588853;
    double age = fmod(days - 2451550.26, SYN);
    if(age < 0) age += SYN;
    double phase = age / SYN;                            /* 0=new .. 0.5=full .. 1=new */
    int illum = (int)((1.0 - cos(2.0 * M_PI * phase)) * 0.5 * 100.0 + 0.5);
    if(illum < 0) illum = 0;
    if(illum > 100) illum = 100;
    if(illum_pct) *illum_pct = illum;
    if(waxing)    *waxing = (phase < 0.5) ? 1 : 0;
    if(name){
        /* eight phases centred on their canonical points */
        int oct = (int)(phase * 8.0 + 0.5) & 7;
        static const char *N[8] = {
            "New", "Waxing crescent", "First quarter", "Waxing gibbous",
            "Full", "Waning gibbous", "Last quarter", "Waning crescent" };
        *name = N[oct];
    }
}

/* ------------------------------------------------------------------- sun times */

#define D2R (M_PI / 180.0)
#define R2D (180.0 / M_PI)

/* Julian day for 12:00 UTC on the given Gregorian date. */
static double julian_noon(int y, int m, int d){
    if(m <= 2){ y -= 1; m += 12; }
    int A = y / 100, B = 2 - A + A / 4;
    return floor(365.25 * (y + 4716)) + floor(30.6001 * (m + 1))
           + d + B - 1524.5 + 0.5;                       /* +0.5 -> noon */
}

void dash_sun_times(int year, int mon, int day, double lat, double lon,
                    int tz_off_min, int *rise_min, int *set_min){
    if(rise_min) *rise_min = -1;
    if(set_min)  *set_min  = -1;

    double jd = julian_noon(year, mon, day);
    double lw = -lon;                                    /* west longitude positive */
    double n  = round(jd - 2451545.0 - 0.0009 + lw / 360.0);
    double Jstar = 2451545.0 + 0.0009 + lw / 360.0 + n;

    double M  = fmod(357.5291 + 0.98560028 * (Jstar - 2451545.0), 360.0);
    double Mr = M * D2R;
    double C  = 1.9148 * sin(Mr) + 0.0200 * sin(2 * Mr) + 0.0003 * sin(3 * Mr);
    double lambda = fmod(M + C + 180.0 + 102.9372, 360.0);
    double lr = lambda * D2R;
    double Jtransit = Jstar + 0.0053 * sin(Mr) - 0.0069 * sin(2 * lr);

    double sinDec = sin(lr) * sin(23.4397 * D2R);
    double cosDec = cos(asin(sinDec));
    double latr = lat * D2R;
    double cosH = (sin(-0.833 * D2R) - sin(latr) * sinDec) / (cos(latr) * cosDec);
    if(cosH > 1.0 || cosH < -1.0) return;                /* polar night / day */

    double H = acos(cosH) * R2D;                         /* half-day arc, degrees */
    double Jrise = Jtransit - H / 360.0;
    double Jset  = Jtransit + H / 360.0;

    /* Julian date -> local minutes since midnight */
    for(int which = 0; which < 2; which++){
        double J = which ? Jset : Jrise;
        double secs = (J - 2440587.5) * 86400.0 + tz_off_min * 60.0;
        long mins = (long)floor(secs / 60.0);
        mins %= 1440; if(mins < 0) mins += 1440;
        if(which){ if(set_min)  *set_min  = (int)mins; }
        else     { if(rise_min) *rise_min = (int)mins; }
    }
}
