/* clock.c -- see clock.h. Epoch persisted in NVS namespace "clock", key "epoch". */
#include "clock.h"
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include "nvs.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "clock";
#define NS  "clock"
#define KEY "epoch"
#define EPOCH_2024 1704067200LL   /* 2024-01-01T00:00:00Z: below this the clock is unset */

void clock_restore(void){
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

/* minimal IANA -> POSIX TZ map (with US/EU DST rules). Extend as needed; an
 * unknown zone (or empty) falls back to UTC so the date is at least stable. */
static const char *iana_to_posix(const char *z){
    if(!z || !z[0]) return "UTC0";
    /* already a POSIX TZ string (e.g. "EST5EDT,M3.2.0,M11.1.0")? pass through. */
    if(strchr(z, ',') || (strlen(z) <= 6 && !strchr(z, '/'))) return z;
    struct { const char *iana, *posix; } M[] = {
        {"America/New_York",    "EST5EDT,M3.2.0,M11.1.0"},
        {"America/Detroit",     "EST5EDT,M3.2.0,M11.1.0"},
        {"America/Chicago",     "CST6CDT,M3.2.0,M11.1.0"},
        {"America/Denver",      "MST7MDT,M3.2.0,M11.1.0"},
        {"America/Phoenix",     "MST7"},
        {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
        {"America/Anchorage",   "AKST9AKDT,M3.2.0,M11.1.0"},
        {"America/Halifax",     "AST4ADT,M3.2.0,M11.1.0"},
        {"Europe/London",       "GMT0BST,M3.5.0/1,M10.5.0"},
        {"Europe/Paris",        "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Berlin",       "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Madrid",       "CET-1CEST,M3.5.0,M10.5.0/3"},
        {"Europe/Athens",       "EET-2EEST,M3.5.0/3,M10.5.0/4"},
        {"Asia/Tokyo",          "JST-9"},
        {"Asia/Shanghai",       "CST-8"},
        {"Asia/Kolkata",        "IST-5:30"},
        {"Australia/Sydney",    "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    };
    for(unsigned i=0;i<sizeof M/sizeof M[0];i++) if(!strcmp(z, M[i].iana)) return M[i].posix;
    return "UTC0";
}

void clock_set_tz(const char *tz){
    const char *posix = iana_to_posix(tz);
    setenv("TZ", posix, 1);
    tzset();
    ESP_LOGI(TAG,"timezone: %s -> %s", (tz&&tz[0])?tz:"(unset)", posix);
}
