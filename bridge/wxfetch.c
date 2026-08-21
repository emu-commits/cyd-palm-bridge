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
    /* forecast_days=2 so the six-hour strip can cross midnight, and timezone=auto
     * so every timestamp comes back in the user's local time -- the dashboard
     * stores local minutes-since-midnight and has no timezone database. */
    int n = snprintf(out, cap,
        OM_HOST "?latitude=%s&longitude=%s"
        "&current=temperature_2m,weather_code"
        "&hourly=temperature_2m,precipitation_probability"
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
    int block = 0;                 /* 1 current, 2 hourly, 3 daily */
    int cur_min = -1;              /* local minutes of the "current" reading */
    char line[256], a[48], b[48];

    while(fgets(line, sizeof line, f)){
        if(line[0] == '\r' || line[0] == '\n' || line[0] == 0){ block = 0; continue; }

        if(header_is(line, "temperature_2m")){
            /* both the current and hourly blocks start with temperature_2m; the
             * third column tells them apart. */
            block = strstr(line, "weather_code") ? 1 : 2;
            continue;
        }
        if(header_is(line, "sunrise")){ block = 3; continue; }
        if(strncmp(line, "latitude,", 9) == 0){ block = 0; continue; }

        if(block == 1){
            if(field(line, 0, a, sizeof a)) cur_min = hhmm_min(a);
            if(field(line, 1, a, sizeof a)) w.cur_tempF = (int16_t)round_dec(a);
            if(field(line, 2, a, sizeof a)) w.cur_code  = (uint8_t)atoi(a);
            have_current = 1;
        } else if(block == 2){
            if(w.nhours >= WX_HOURS) continue;
            if(!field(line, 0, a, sizeof a)) continue;
            int hm = hhmm_min(a);
            /* keep the hours still ahead of the current reading; the feed starts
             * at local midnight, so most of the first day is already behind us. */
            if(cur_min >= 0 && hm >= 0 && hm <= cur_min && w.nhours == 0) continue;
            int h = hour_of(a);
            if(h < 0) continue;
            w.hr[w.nhours].hour24 = (uint8_t)h;
            if(field(line, 1, b, sizeof b)) w.hr[w.nhours].tempF = (int16_t)round_dec(b);
            if(field(line, 2, b, sizeof b)){ int r = atoi(b); w.hr[w.nhours].rain =
                                             (uint8_t)(r < 0 ? 0 : r > 100 ? 100 : r); }
            w.nhours++;
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
