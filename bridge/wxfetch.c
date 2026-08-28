/* wxfetch.c -- see wxfetch.h. Line-at-a-time, no heap, no JSON. */
#include "wxfetch.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define OM_HOST "https://api.open-meteo.com/v1/forecast"
#define AQ_HOST "https://air-quality-api.open-meteo.com/v1/air-quality"

/* A coordinate we are willing to paste into a URL: optional sign, digits, at
 * most one dot. Anything else (a stray comment, "N/A", a degree symbol) is
 * rejected here rather than sent to the server and diagnosed as a 400. */
static int coord_ok(const char *s){
    if(!s || !s[0]) return 0;
    int digits = 0, dots = 0, i = 0;
    if(s[0]=='+' || s[0]=='-') i = 1;
    for(; s[i]; i++){
        if(s[i]=='.'){ if(++dots > 1) return 0; continue; }
        if(!isdigit((unsigned char)s[i])) return 0;
        digits++;
    }
    return digits > 0;
}

int wx_build_url(char *out, int cap, const char *lat, const char *lon){
    if(!out || cap <= 0) return 0;
    out[0] = 0;
    if(!coord_ok(lat) || !coord_ok(lon)) return 0;
    /* forecast_days=2 so a full day of hourly rows is available whatever time the
     * sync runs, and timezone=auto so every timestamp comes back in the user's
     * local time -- the dashboard stores local minutes-since-midnight and has no
     * timezone database.
     *
     * hourly weather_code is requested as well as the current one: the cache is
     * stepped through as the day passes (WX_HOURS), and a temperature that
     * advances under a description frozen at sync time is worse than either --
     * "81 degrees, Light rain" at six in the evening because it rained at nine
     * in the morning. */
    int n = snprintf(out, cap,
        OM_HOST "?latitude=%s&longitude=%s"
        "&current=temperature_2m,weather_code"
        "&hourly=temperature_2m,precipitation_probability,weather_code"
        "&daily=sunrise,sunset"
        "&temperature_unit=fahrenheit&timezone=auto&forecast_days=2&format=csv",
        lat, lon);
    if(n < 0 || n >= cap){ out[0] = 0; return 0; }
    return n;
}

int wx_build_aqi_url(char *out, int cap, const char *lat, const char *lon){
    if(!out || cap <= 0) return 0;
    out[0] = 0;
    if(!coord_ok(lat) || !coord_ok(lon)) return 0;
    int n = snprintf(out, cap,
        AQ_HOST "?latitude=%s&longitude=%s&current=us_aqi&timezone=auto&format=csv",
        lat, lon);
    if(n < 0 || n >= cap){ out[0] = 0; return 0; }
    return n;
}

/* ---- CSV helpers ---------------------------------------------------------- */

/* the k-th comma-separated field of `line`, copied into out[cap]. 1 if present. */
static int field(const char *line, int k, char *out, int cap){
    const char *p = line;
    for(int i = 0; i < k; i++){
        p = strchr(p, ',');
        if(!p) return 0;
        p++;
    }
    const char *e = strchr(p, ',');
    int len = e ? (int)(e - p) : (int)strcspn(p, "\r\n");
    if(len > cap - 1) len = cap - 1;
    if(len < 0) len = 0;
    memcpy(out, p, len);
    out[len] = 0;
    return 1;
}

/* round a decimal string to the nearest whole number, without floating point in
 * the caller's path: "68.4" -> 68, "-3.6" -> -4. */
static int round_dec(const char *s){
    int neg = 0, i = 0;
    if(s[0]=='-'){ neg = 1; i = 1; } else if(s[0]=='+') i = 1;
    long whole = 0;
    for(; s[i] && s[i] != '.'; i++){
        if(!isdigit((unsigned char)s[i])) return 0;
        whole = whole * 10 + (s[i] - '0');
    }
    if(s[i] == '.' && isdigit((unsigned char)s[i+1]) && s[i+1] >= '5') whole++;
    return neg ? (int)-whole : (int)whole;
}

/* "2026-08-20T19:30" -> local minutes since midnight, or -1. */
static int hhmm_min(const char *iso){
    const char *t = strchr(iso, 'T');
    if(!t || !isdigit((unsigned char)t[1]) || !isdigit((unsigned char)t[4])) return -1;
    int h = (t[1]-'0')*10 + (t[2]-'0');
    int m = (t[4]-'0')*10 + (t[5]-'0');
    if(h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

/* the hour field of "2026-08-20T19:00", or -1 */
static int hour_of(const char *iso){
    int m = hhmm_min(iso);
    return m < 0 ? -1 : m / 60;
}

/* "2026-08-20T19:00" + the feed's UTC offset -> Unix time, or 0 on a bad stamp.
 * timegm() is not portable and mktime() would apply the DEVICE's zone to a
 * timestamp that is in the FEED's zone, so the civil-date arithmetic is done here
 * (days-from-civil; integer, no libc, no zone database). */
static int64_t iso_epoch(const char *iso, int off_sec){
    int y,mo,d,h,mi;
    if(!iso) return 0;
    if(sscanf(iso, "%4d-%2d-%2dT%2d:%2d", &y,&mo,&d,&h,&mi) != 5) return 0;
    if(mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
    int64_t yy = y - (mo <= 2);
    int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
    int64_t yoe = yy - era * 400;                                   /* [0, 399] */
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365] */
    int64_t doe = yoe * 365 + yoe/4 - yoe/100 + doy;                /* [0, 146096] */
    int64_t days = era * 146097 + doe - 719468;                     /* since 1970-01-01 */
    return days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 - (int64_t)off_sec;
}

/* Does this header row name the block we want? Matched on the SECOND column's
 * name, which is what distinguishes the three time-series blocks. */
static int header_is(const char *line, const char *col2){
    if(strncmp(line, "time,", 5) != 0) return 0;
    return strncmp(line + 5, col2, strlen(col2)) == 0;
}

/* Open-Meteo's own local timestamps are what we compare against, so "is this
 * hour still ahead of us" is answered in local terms too: the current block's
 * timestamp is the device's local now, per timezone=auto. */
int wx_parse_file(const char *path, int64_t now, WxCache *out){
    FILE *f = fopen(path, "rb");
    if(!f) return 0;

    WxCache w;
    memset(&w, 0, sizeof w);
    w.magic       = WX_MAGIC;
    w.gen_epoch   = now;
    w.aqi         = -1;
    w.sunrise_min = -1;
    w.sunset_min  = -1;
    w.cur_tempF   = 0;
    w.nhours      = 0;

    int have_current = 0;
    int block = 0;                 /* 1 current, 2 hourly, 3 daily, 4 meta */
    int off_sec = 0, have_off = 0; /* the feed's UTC offset, from the meta block */
    int cur_min = -1;              /* local minutes of the "current" reading */
    char line[256], a[48], b[48];

    while(fgets(line, sizeof line, f)){
        if(line[0] == '\r' || line[0] == '\n' || line[0] == 0){ block = 0; continue; }

        if(header_is(line, "temperature_2m")){
            /* Both the current and hourly blocks start with temperature_2m. They
             * used to be told apart by weather_code, which only the current block
             * carried -- until the hourly block started carrying it too. The
             * discriminator is now precipitation_probability, which is asked for
             * ONLY in the hourly block, so the test cannot go stale the same way. */
            block = strstr(line, "precipitation_probability") ? 2 : 1;
            continue;
        }
        if(header_is(line, "sunrise")){ block = 3; continue; }
        if(strncmp(line, "latitude,", 9) == 0){ block = 4; continue; }

        if(block == 1){
            if(field(line, 0, a, sizeof a)) cur_min = hhmm_min(a);
            if(field(line, 1, a, sizeof a)) w.cur_tempF = (int16_t)round_dec(a);
            if(field(line, 2, a, sizeof a)) w.cur_code  = (uint8_t)atoi(a);
            have_current = 1;
        } else if(block == 2){
            if(w.nhours >= WX_HOURS) continue;
            if(!field(line, 0, a, sizeof a)) continue;
            int hm = hhmm_min(a);
            /* keep the hours still ahead of the current reading; the feed starts at
             * local midnight, so most of the first day is already behind us. */
            if(cur_min >= 0 && hm >= 0 && hm <= cur_min && w.nhours == 0) continue;
            int h = hour_of(a);
            if(h < 0) continue;
            if(w.nhours == 0){
                /* The row's own timestamp is the anchor the dashboard steps from.
                 * Without the feed's UTC offset there is no honest way to turn a
                 * local timestamp into an epoch, so leave it 0 and let
                 * dash_wx_index_at() refuse rather than guess a zone. */
                if(have_off) w.hr0_epoch = iso_epoch(a, off_sec);
            }
            w.hr[w.nhours].hour24 = (uint8_t)h;
            if(field(line, 1, b, sizeof b)) w.hr[w.nhours].tempF = (int16_t)round_dec(b);
            if(field(line, 2, b, sizeof b)){ int r = atoi(b); w.hr[w.nhours].rain =
                                             (uint8_t)(r < 0 ? 0 : r > 100 ? 100 : r); }
            /* Optional: a feed without hourly weather_code leaves this 0, which
             * dash_wx_now() reads as "no code for this hour" and falls back to the
             * fetch-time one. Absence is handled, not assumed away. */
            if(field(line, 3, b, sizeof b)){ int c = atoi(b);
                                             w.hr[w.nhours].code = (uint8_t)(c < 0 || c > 255 ? 0 : c); }
            w.nhours++;
        } else if(block == 4){
            /* latitude,longitude,elevation,utc_offset_seconds,timezone,abbrev */
            if(field(line, 3, a, sizeof a)){ off_sec = atoi(a); have_off = 1; }
            block = 0;                       /* one data row, then the block ends */
        } else if(block == 3){
            /* the first daily row is today; later rows are tomorrow, ignored */
            if(w.sunrise_min >= 0) continue;
            if(field(line, 1, a, sizeof a)) w.sunrise_min = (int16_t)hhmm_min(a);
            if(field(line, 2, a, sizeof a)) w.sunset_min  = (int16_t)hhmm_min(a);
        }
    }
    fclose(f);
    if(!have_current) return 0;
    *out = w;
    return 1;
}

int wx_parse_aqi_file(const char *path){
    FILE *f = fopen(path, "rb");
    if(!f) return -1;
    int aqi = -1, block = 0;
    char line[256], a[48];
    while(fgets(line, sizeof line, f)){
        if(line[0] == '\r' || line[0] == '\n' || line[0] == 0){ block = 0; continue; }
        if(header_is(line, "us_aqi")){ block = 1; continue; }
        if(block == 1 && field(line, 1, a, sizeof a) && a[0]){
            aqi = atoi(a);
            if(aqi < 0 || aqi > 1000) aqi = -1;
            break;
        }
    }
    fclose(f);
    return aqi;
}
