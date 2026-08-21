/* clock.c -- see clock.h. Epoch persisted in NVS namespace "clock", key "epoch". */
#include "clock.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "nvs.h"
#include "esp_timer.h"
#include "esp_system.h"   /* esp_reset_reason() -- did this boot lose the clock? */
#include "esp_log.h"

static const char *TAG = "clock";
#define NS  "clock"
#define KEY "epoch"
#define EPOCH_2024 1704067200LL   /* 2024-01-01T00:00:00Z: below this the clock is unset */

/* drift meter (see clock.h) */
#define KEY_ANCH   "syncanch"     /* usec-since-epoch of the last counted sync  */
#define KEY_ANCHL  "syncanchl"    /* the clock-loss count as of that sync       */
#define KEY_LOST   "clklost"      /* boots that started without a running clock */
#define DRIFT_LOG  "/sdcard/drift.log"
#define DRIFT_MIN_S 600           /* below this SNTP's own error dominates      */
#ifdef CLOCK_TEST_HOOKS
#define DRIFT_HOOK                /* visible to the host gate                   */
#else
#define DRIFT_HOOK static
#endif

/* Did this boot start from a state where the RTC domain -- and with it the system
 * clock -- was gone? A software, panic or watchdog reset leaves the RTC timer
 * running and ESP-IDF carries the time across it. A power-on, a brown-out, or the
 * EN-pin RESET key does not. The drift meter has to know the difference: a
 * correction measured across one of those is the length of the outage, not drift. */
static int boot_lost_the_clock(void){
    switch(esp_reset_reason()){
        case ESP_RST_POWERON: case ESP_RST_BROWNOUT:
        case ESP_RST_EXT:     case ESP_RST_UNKNOWN: return 1;
        default:                                    return 0;
    }
}

static void count_clock_loss(void){
    if(!boot_lost_the_clock()) return;
    nvs_handle_t h;
    if(nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t n = 0;
    nvs_get_u32(h, KEY_LOST, &n);
    nvs_set_u32(h, KEY_LOST, n + 1);
    nvs_commit(h);
    nvs_close(h);
}

void clock_restore(void){
    count_clock_loss();          /* before the early return below: it must always run */
    nvs_handle_t h;
    if(nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;   /* nothing saved yet */
    uint64_t ep = 0;
    if(nvs_get_u64(h, KEY, &ep) == ESP_OK && (long long)ep >= EPOCH_2024){
        struct timeval tv = { .tv_sec = (time_t)ep, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        struct tm ti; time_t t=(time_t)ep; localtime_r(&t,&ti);
        ESP_LOGI(TAG,"restored clock from NVS: %04d-%02d-%02d %02d:%02d (local)",
                 ti.tm_year+1900,ti.tm_mon+1,ti.tm_mday,ti.tm_hour,ti.tm_min);
    }
    nvs_close(h);
}

void clock_checkpoint(void){
    time_t now = 0; time(&now);
    if((long long)now < EPOCH_2024) return;                /* clock not set -> don't persist 1970 */
    nvs_handle_t h;
    if(nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint64_t prev = 0;
    /* only rewrite if it moved forward by >=30 s -> avoids needless flash wear */
    if(nvs_get_u64(h, KEY, &prev) != ESP_OK || (long long)now - (long long)prev >= 30){
        nvs_set_u64(h, KEY, (uint64_t)now);
        nvs_commit(h);
    }
    nvs_close(h);
}

static void tick(void *arg){ (void)arg; clock_checkpoint(); }
void clock_start_autosave(void){
    const esp_timer_create_args_t a = { .callback = tick, .name = "clockchk" };
    esp_timer_handle_t t;
    if(esp_timer_create(&a, &t) == ESP_OK)
        esp_timer_start_periodic(t, 120ULL * 1000 * 1000);  /* every 120 s */
}

/* ------------------------------------------------------------- drift meter */
static struct timeval s_before;      /* the clock as it stood before SNTP        */
static int64_t        s_mark;        /* monotonic us at the same instant         */
static int            s_open;        /* begin() called, end() not yet            */

static uint32_t loss_count(void){
    nvs_handle_t h; uint32_t n = 0;
    if(nvs_open(NS, NVS_READONLY, &h) == ESP_OK){ nvs_get_u32(h, KEY_LOST, &n); nvs_close(h); }
    return n;
}

/* One place to say it, so the serial log and the SD log can never disagree. */
static void drift_report(const char *msg){
    ESP_LOGI(TAG, "drift: %s", msg);
    FILE *f = fopen(DRIFT_LOG, "a");
    if(!f) return;
    time_t now = 0; time(&now); struct tm ti; localtime_r(&now, &ti);
    fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  %s\n",
            ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec, msg);
    fclose(f);
}

/* Build the one-line summary for a sample. All the arithmetic that can be wrong
 * silently lives here -- the sign convention, the ppm identity, the integer
 * scaling -- with none of the NVS or SNTP plumbing, so `make -C sim clock` can
 * pin it. `lost` is how many times the clock stopped inside the interval.
 * Not static under CLOCK_TEST_HOOKS; see sim/tests/clock_test.c. */
DRIFT_HOOK void drift_line(char *out, int cap, int64_t corr_us, long long span_s,
                           unsigned lost){
    long long hrs = span_s / 3600, mins = (span_s % 3600) / 60;
    long long ms  = (long long)(corr_us / 1000);

    if(lost){
        snprintf(out, cap,
                 "SNTP moved the clock %+lld ms over %lldh%02lldm, but the clock stopped "
                 "%u time(s) in between -- that is the outage, not drift", ms, hrs, mins, lost);
        return;
    }
    if(span_s < DRIFT_MIN_S){
        snprintf(out, cap, "%+lld ms over %llds -- too short to mean anything", ms, span_s);
        return;
    }
    /* ppm is exactly corr_us/span_s, which is why none of this needs floating
     * point (ESP_LOG cannot be trusted with %f under the nano formatter). */
    long long ppm100 = corr_us * 100 / span_s;
    long long day100 = corr_us * 864 / (span_s * 100);
    long long a_ppm  = ppm100 < 0 ? -ppm100 : ppm100;
    long long a_day  = day100 < 0 ? -day100 : day100;
    snprintf(out, cap,
             "%+lld ms over %lldh%02lldm -> clock runs %s by %lld.%02lld ppm (%lld.%02lld s/day)",
             ms, hrs, mins, corr_us > 0 ? "SLOW" : "FAST",
             a_ppm / 100, a_ppm % 100, a_day / 100, a_day % 100);
}

void clock_sync_begin(void){
    gettimeofday(&s_before, NULL);
    s_mark = esp_timer_get_time();
    s_open = 1;
}

void clock_sync_end(int synced){
    char msg[168];
    if(!s_open) return;
    s_open = 0;
    if(!synced) return;                       /* nothing was corrected -> no sample */

    struct timeval after; gettimeofday(&after, NULL);
    /* What SNTP actually changed the clock by. The wall clock also advanced by the
     * real time spent waiting for the server, which is not error -- the monotonic
     * mark is the only way to subtract it. */
    int64_t waited = esp_timer_get_time() - s_mark;
    int64_t moved  = ((int64_t)after.tv_sec  - (int64_t)s_before.tv_sec) * 1000000
                   + ((int64_t)after.tv_usec - (int64_t)s_before.tv_usec);
    int64_t corr   = moved - waited;
    int64_t now_us = (int64_t)after.tv_sec * 1000000 + (int64_t)after.tv_usec;

    /* Read the old anchor and lay the new one in the same open. Re-anchoring
     * unconditionally means a nonsense anchor can spoil one sample, never a run. */
    int64_t  anchor = 0;
    uint32_t anchor_loss = 0, now_loss = loss_count();
    int      have = 0;
    nvs_handle_t h;
    if(nvs_open(NS, NVS_READWRITE, &h) == ESP_OK){
        uint64_t a = 0;
        if(nvs_get_u64(h, KEY_ANCH, &a) == ESP_OK && a){ anchor = (int64_t)a; have = 1; }
        nvs_get_u32(h, KEY_ANCHL, &anchor_loss);
        nvs_set_u64(h, KEY_ANCH,  (uint64_t)now_us);
        nvs_set_u32(h, KEY_ANCHL, now_loss);
        nvs_commit(h);
        nvs_close(h);
    }

    if(!have){
        drift_report("anchor set; the next sync measures the interval");
        return;
    }
    int64_t span = now_us - anchor;
    if(span <= 0 || span > 365LL * 86400 * 1000000){
        snprintf(msg, sizeof msg, "anchor was nonsense (%lld s span); re-anchored",
                 (long long)(span / 1000000));
        drift_report(msg);
        return;
    }

    drift_line(msg, sizeof msg, corr, (long long)(span / 1000000),
               (unsigned)(now_loss - anchor_loss));
    drift_report(msg);
}

/* Built-in IANA -> POSIX TZ map (with US/EU/AU DST rules). The POSIX string
 * carries the DST transition rules, so once tzset() sees it localtime() applies
 * DST automatically by the current date -- no separate DST logic needed. This
 * table is also the source for the on-device timezone picker (clock_zone_*),
 * which is why it lives at file scope. Extend as needed. */
static const struct { const char *iana, *posix; } TZ_TBL[] = {
    {"America/New_York",    "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Detroit",     "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago",     "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver",      "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Phoenix",     "MST7"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"America/Anchorage",   "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America/Halifax",     "AST4ADT,M3.2.0,M11.1.0"},
    {"America/Sao_Paulo",   "BRT3"},
    {"UTC",                 "UTC0"},
    {"Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Dublin",       "GMT0IST,M3.5.0/1,M10.5.0"},
    {"Europe/Paris",        "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid",       "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Athens",       "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Moscow",       "MSK-3"},
    {"Asia/Dubai",          "GST-4"},
    {"Asia/Kolkata",        "IST-5:30"},
    {"Asia/Shanghai",       "CST-8"},
    {"Asia/Tokyo",          "JST-9"},
    {"Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Pacific/Auckland",    "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};
#define TZ_TBL_N ((int)(sizeof TZ_TBL / sizeof TZ_TBL[0]))

/* --- zone resolution -------------------------------------------------------
 * An unrecognised zone falls back to UTC so the date is at least stable, but
 * SILENTLY falling back is how a device ends up confidently showing the wrong
 * hour: the clock is right, the label says the zone, and only the number is
 * wrong. So resolution now reports whether it actually recognised the zone, the
 * caller says so out loud, and the matching itself is forgiving of the ways a
 * person actually types a zone into config.ini. */

static char s_tz_in[48];      /* what was asked for */
static char s_tz_posix[48];   /* what is in force   */
static int  s_tz_known;       /* 0 => the fallback is running, not the request */

static int ci_eq(const char *a, const char *b){
    for(; *a && *b; a++, b++){
        char x=*a, y=*b;
        if(x>='A'&&x<='Z') x += 32;
        if(y>='A'&&y<='Z') y += 32;
        if(x=='_') x=' ';                    /* "New_York" == "New York" */
        if(y=='_') y=' ';
        if(x!=y) return 0;
    }
    return *a==0 && *b==0;
}

/* The leading alphabetic run of a POSIX TZ string is its standard-time
 * abbreviation ("EST" of "EST5EDT,..."); the run after the offset is the DST one
 * ("EDT"). Both are things people type. */
static int abbrev_eq(const char *posix, const char *z, int want_dst){
    char ab[8]; int n=0, i=0;
    while(posix[i] && !((posix[i]>='0'&&posix[i]<='9') || posix[i]=='-' || posix[i]=='+')){
        if(n < (int)sizeof ab - 1) ab[n++] = posix[i];
        i++;
    }
    if(!want_dst){ ab[n]=0; return n>0 && ci_eq(ab, z); }
    while(posix[i] && ((posix[i]>='0'&&posix[i]<='9')||posix[i]=='-'||posix[i]=='+'||posix[i]==':')) i++;
    n=0;
    while(posix[i] && posix[i] != ','){ if(n < (int)sizeof ab - 1) ab[n++]=posix[i]; i++; }
    ab[n]=0;
    return n>0 && ci_eq(ab, z);
}

/* "America/New_York" -> "New_York"; a zone with no '/' is its own tail. */
static const char *tail(const char *z){
    const char *p = strrchr(z, '/');
    return p ? p+1 : z;
}

/* A real POSIX TZ carries an OFFSET after the abbreviation ("EST5", "CET-1") or
 * DST rules after a comma. A bare "EST" does NOT -- newlib reads it as UTC, which
 * is exactly the silent five-hour error this check exists to prevent. */
static int looks_posix(const char *z){
    if(strchr(z, ',')) return 1;
    int i=0;
    while(z[i] && ((z[i]>='A'&&z[i]<='Z')||(z[i]>='a'&&z[i]<='z'))) i++;
    if(i < 3) return 0;
    return z[i]=='-' || z[i]=='+' || (z[i]>='0' && z[i]<='9');
}

/* Resolve `z` to a POSIX TZ string. Returns 1 if it was recognised, 0 if the
 * result is the UTC fallback. An empty zone is a deliberate choice (floating
 * local time), so it counts as recognised. */
DRIFT_HOOK int tz_resolve(const char *z, const char **out){
    *out = "UTC0";
    if(!z || !z[0]) return 1;
    if(looks_posix(z)){ *out = z; return 1; }
    for(int i=0;i<TZ_TBL_N;i++) if(!strcmp(z, TZ_TBL[i].iana)){ *out = TZ_TBL[i].posix; return 1; }
    /* case/underscore-insensitive, then by the city alone, then by abbreviation */
    for(int i=0;i<TZ_TBL_N;i++) if(ci_eq(z, TZ_TBL[i].iana)){ *out = TZ_TBL[i].posix; return 1; }
    for(int i=0;i<TZ_TBL_N;i++) if(ci_eq(z, tail(TZ_TBL[i].iana))){ *out = TZ_TBL[i].posix; return 1; }
    for(int i=0;i<TZ_TBL_N;i++) if(abbrev_eq(TZ_TBL[i].posix, z, 0)){ *out = TZ_TBL[i].posix; return 1; }
    for(int i=0;i<TZ_TBL_N;i++) if(abbrev_eq(TZ_TBL[i].posix, z, 1)){ *out = TZ_TBL[i].posix; return 1; }
    return 0;
}

static const char *iana_to_posix(const char *z){
    const char *p; tz_resolve(z, &p); return p;
}

int clock_zone_count(void){ return TZ_TBL_N; }
const char *clock_zone_name(int i){ return (i>=0 && i<TZ_TBL_N) ? TZ_TBL[i].iana : ""; }

void clock_now_desc(char *out, int cap){
    if(!out || cap<=0) return;
    time_t now=0; time(&now);
    struct tm ti; localtime_r(&now,&ti);
    /* %Z = zone abbrev (EDT), %z = numeric offset (-0400) under the active TZ. */
    char zbuf[24]="";
    strftime(zbuf,sizeof zbuf,"%Z %z",&ti);
    snprintf(out,cap,"%s %s", zbuf, ti.tm_isdst>0 ? "(DST)" : "(standard)");
}

void clock_set_tz(const char *tz){
    const char *posix = NULL;
    s_tz_known = tz_resolve(tz, &posix);
    snprintf(s_tz_in,    sizeof s_tz_in,    "%s", (tz && tz[0]) ? tz : "");
    snprintf(s_tz_posix, sizeof s_tz_posix, "%s", posix);
    setenv("TZ", posix, 1);
    tzset();
    if(s_tz_known)
        ESP_LOGI(TAG,"timezone: %s -> %s", s_tz_in[0] ? s_tz_in : "(unset)", posix);
    else
        ESP_LOGW(TAG,"timezone \"%s\" not recognised -- running on UTC. Use an IANA "
                     "name from the built-in list (e.g. America/New_York) or a POSIX "
                     "TZ string (e.g. EST5EDT,M3.2.0,M11.1.0).", s_tz_in);
}

int clock_tz_known(void){ return s_tz_known; }
const char *clock_tz_posix(void){ return s_tz_posix; }

void clock_zone_hhmm(const char *iana, time_t t, char *out, int cap){
    if(!out || cap <= 0) return;
    out[0] = '\0';
    /* apply the target zone just long enough to format its wall clock, then put the
     * user's active zone back so the rest of the UI (and clock_now_desc) is unaffected. */
    char save[48] = "";
    const char *cur = getenv("TZ");
    if(cur){ strncpy(save, cur, sizeof save - 1); }
    setenv("TZ", iana_to_posix(iana), 1); tzset();
    struct tm ti; localtime_r(&t, &ti);
    strftime(out, cap, "%H:%M", &ti);
    if(save[0]) setenv("TZ", save, 1); else unsetenv("TZ");
    tzset();
}
