/* hotsync.c -- background sync to iCloud (U7).
 *
 * Reuses the proven engine (dav.h + sync.h). Runs on its own task with a live
 * status string the UI polls. DEFENSIVE: no ESP_ERROR_CHECK -- every step checks
 * its return and fails to a status message, so a network/RAM problem can't crash
 * the interactive UI. Wi-Fi is brought up only for the sync (the roadmap's
 * mode-switch); RAM headroom for Wi-Fi+TLS+sync while LVGL is up is the thing to
 * validate on-device -- if it's tight, tear down the LVGL draw buffer first.
 */
#include "hotsync.h"
#include "dav.h"
#include "sync.h"
#include "rss.h"          /* RSS reader: feed parser */
#include "news.h"         /* RSS reader: on-SD article store */
#include "feeds.h"        /* RSS reader: the enabled feed sources */
#include "dash.h"         /* lock-screen dashboard: the WxCache it renders from */
#include "wxfetch.h"      /* Open-Meteo CSV -> WxCache */
#include "secrets.h"
#include "appcfg.h"
#include "clock.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "hotsync";

/* The sync task's stack comes out of the same heap the TLS handshake needs, and
 * at 32 KB it was taking a third of what was free. Two things paid for the cut:
 * rss.c's emit_item no longer puts 8.7 KB of buffers on this stack (they are BSS
 * now), and the end-of-run high-water log below reports what is actually used,
 * so this number stays honest. Raise it only against that log. */
#define HOTSYNC_STACK 20480
static volatile int s_busy;
static char s_status[208] = "Ready";
static void setst(const char *s){ snprintf(s_status, sizeof s_status, "%s", s); }

/* ---- Mode B headroom probe -----------------------------------------------
 * BACKLOG: "Measure Mode B headroom with the UI resident". The number that matters
 * -- free RAM at the mbedTLS handshake peak while LVGL is still fully resident --
 * occurs INSIDE the handshake, where no log line can sit. So bracket it instead:
 * min_ever (esp_get_minimum_free_heap_size) is a since-boot low-water mark and only
 * ever falls, so if it drops between the "pre-tls" and "post-tls" samples, that
 * lower value IS the peak.
 * READING IT: min_ever is since BOOT, not since the sync. If it does NOT move
 * pre->post, that does not mean the handshake was free -- it means the handshake
 * never dipped below some earlier boot transient, and this run tells you nothing
 * about the peak. Reboot and sync immediately to get a clean bracket. Note also
 * that the `free` column at "post-tls" is sampled after the handshake buffers are
 * released, so it OVERSTATES headroom; it is a lower bound on the peak, not the
 * peak. largestDMA answers the separate question of whether a
 * Wi-Fi up/down cycle fragments the DMA-capable region that LVGL's ~19 KB
 * *contiguous* draw buffer depends on. Cheap and INFO-level: this runs a handful of
 * times per sync, and the answer decides whether the never-built draw-buffer
 * teardown is needed. */
static void hs_heap(const char *where){
    ESP_LOGI(TAG,"heap[%s]: free=%lu min_ever=%lu largest8=%lu largestDMA=%lu",
             where,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

/* sync progress 0..100 for the UI status line. Each configured collection owns a
 * band [s_band_lo, s_band_lo+s_band_span]; the engine's per-record progress hook
 * (hs_prog_cb) fills that band, so the bar advances WITHIN a collection, not just
 * as each starts. -1 = idle/not started. */
static volatile int s_prog = -1;
static int s_band_lo, s_band_span;
static void setprog(int p){ s_prog = p; }
int hotsync_progress(void){ return s_prog; }
/* engine progress hook: map done/total into the current collection's band. Runs
 * on the sync task; s_prog is a plain int read by the LVGL task (atomic write). */
static void hs_prog_cb(int done,int total,void *ctx){
    (void)ctx;
    int frac = (total > 0) ? (done * 100 / total) : 0;
    if(frac > 100) frac = 100;                 /* server-only pulls can exceed total */
    s_prog = s_band_lo + frac * s_band_span / 100;
}

/* ---- per-app sync targets ------------------------------------------------
 * HotSync walks this table, syncing each configured app to its own iCloud
 * collection. The PDB/map paths and kinds are device-local constants; the
 * COLLECTION for each app is now runtime config (appcfg(): config.ini over the
 * secrets.h seed), so an app with an empty collection stays off until it's
 * configured -- exactly the old back-compat behaviour, now editable on-device. */
#ifndef SYNC_TODO_PDB
#define SYNC_TODO_PDB "/sdcard/ToDoDB.pdb"
#endif
#ifndef SYNC_CARD_PDB
#define SYNC_CARD_PDB "/sdcard/AddressDB.pdb"
#endif

typedef struct {
    const char *name;     /* label for the status line            */
    const char *pdb;      /* PDB path on SD                        */
    int         kind;     /* KIND_CAL | KIND_TODO | KIND_CARD      */
    const char *map;      /* per-collection state map on SD        */
    int         card;     /* 1 => CardDAV (uses the contacts host) */
} SyncApp;

static const SyncApp s_apps[] = {
    { "Date Book", SYNC_PDB,      KIND_CAL,  "/sdcard/state/cal.map",  0 },
    { "To Do",     SYNC_TODO_PDB, KIND_TODO, "/sdcard/state/todo.map", 0 },
    { "Address",   SYNC_CARD_PDB, KIND_CARD, "/sdcard/state/card.map", 1 },
};
#define N_APPS ((int)(sizeof s_apps / sizeof s_apps[0]))

/* the configured collection for app i (index matches s_apps order). */
static const char* app_coll(const Config *c, int i){
    return i==0 ? c->cal_coll : i==1 ? c->todo_coll : c->card_coll;
}

int hotsync_busy(void){ return s_busy; }
const char *hotsync_status(void){ return s_status; }

/* ---- Wi-Fi (defensive; brought up per sync) ---- */
static EventGroupHandle_t s_evt;
#define WIFI_OK   BIT0
#define WIFI_FAIL BIT1
static int s_retries;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;
static int s_netif_inited;
static esp_netif_t *s_netif;

static void wifi_ev(void *a, esp_event_base_t base, int32_t id, void *data){
    (void)a;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START) esp_wifi_connect();
    else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED){
        if(s_retries++ < 8) esp_wifi_connect();
        else xEventGroupSetBits(s_evt, WIFI_FAIL);
    } else if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP){
        s_retries=0; xEventGroupSetBits(s_evt, WIFI_OK);
    }
}

static int wifi_up(void){
    s_evt = xEventGroupCreate();
    if(esp_netif_init()!=ESP_OK) return 0;
    if(esp_event_loop_create_default()!=ESP_OK){ /* may already exist */ }
    if(!s_netif) s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if(esp_wifi_init(&cfg)!=ESP_OK) return 0;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_ev, NULL, &s_h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_ev, NULL, &s_h_ip);
    const Config *pc = appcfg();
    wifi_config_t wc = { 0 };
    strncpy((char*)wc.sta.ssid, pc->wifi_ssid, sizeof wc.sta.ssid);
    strncpy((char*)wc.sta.password, pc->wifi_pass, sizeof wc.sta.password);
    if(esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK) return 0;
    if(esp_wifi_set_config(WIFI_IF_STA, &wc)!=ESP_OK) return 0;
    if(esp_wifi_start()!=ESP_OK) return 0;
    EventBits_t b = xEventGroupWaitBits(s_evt, WIFI_OK|WIFI_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(25000));
    (void)s_netif_inited;
    return (b & WIFI_OK) ? 1 : 0;
}
/* ---- what the network actually gave us -------------------------------------
 * Three internet stages (SNTP, iCloud, news feeds) all failing at once is one
 * fault, not three, and the usual single fault is name resolution: DHCP handed
 * out an address but no usable DNS, so every hostname dies before a packet
 * leaves. Nothing above could tell that apart from "the server is down", so
 * print the lease and resolve one name before any of them run. Cheap: one UDP
 * round trip, once per sync. */
static void net_probe(void){
    esp_netif_ip_info_t ip; memset(&ip,0,sizeof ip);
    if(s_netif && esp_netif_get_ip_info(s_netif,&ip)==ESP_OK)
        ESP_LOGI(TAG,"lease: ip=" IPSTR " gw=" IPSTR " mask=" IPSTR,
                 IP2STR(&ip.ip), IP2STR(&ip.gw), IP2STR(&ip.netmask));

    for(int i=0;i<2;i++){
        esp_netif_dns_info_t dns; memset(&dns,0,sizeof dns);
        if(s_netif && esp_netif_get_dns_info(s_netif,
               i ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &dns)==ESP_OK)
            ESP_LOGI(TAG,"dns[%s]: " IPSTR, i?"backup":"main",
                     IP2STR(&dns.ip.u_addr.ip4));
    }

    /* the resolve itself: if this fails, nothing below can possibly work */
    const char *probe = "pool.ntp.org";
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(probe, "123", &hints, &res);
    if(rc == 0 && res){
        struct in_addr a = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG,"dns probe: %s -> %s", probe, inet_ntoa(a));
        freeaddrinfo(res);
    } else {
        ESP_LOGE(TAG,"dns probe: %s did NOT resolve (rc=%d) -- the clock, iCloud "
                     "and the news feeds will all fail for this one reason", probe, rc);
    }
}

static void wifi_down(void){
    dav_disconnect();   /* close any keep-alive connection before the TLS/socket stack goes away */
    esp_wifi_stop();
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
    esp_wifi_deinit();
    if(s_evt){ vEventGroupDelete(s_evt); s_evt=NULL; }
}

/* Did the LAST clock_ok() actually hear back from a time server? A plausible
 * clock is not a synced clock: the NVS checkpoint restored at boot is always
 * "some time in 2024 or later" and so always passes the sanity test, which is
 * how a device can report a successful HotSync and still show a time that is
 * hours out. The two answers are now separate, and the caller says which it got. */
static int s_clock_synced;

static int clock_ok(void){
    /* Three servers, not one: a single name that will not resolve (or whose NTP
     * the router blocks) is otherwise indistinguishable from "the clock is fine",
     * and pool.ntp.org is exactly the name most likely to be intercepted. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
        ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.cloudflare.com", "time.google.com"));
    s_clock_synced = 0;
    if(esp_netif_sntp_init(&cfg)!=ESP_OK){
        ESP_LOGE(TAG,"SNTP init failed");
        return 0;
    }
    time_t before=0; time(&before);
    /* Every sync is a free measurement of how far the clock wandered since the
     * last one -- the number that decides whether this device needs an RTC part.
     * The bracket has to be OUTSIDE the wait, because the correction is only
     * meaningful against the clock as it stood before SNTP touched it. */
    clock_sync_begin();
    int synced = 0;
    for(int i=0;i<15;i++)
        if(esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000))==ESP_OK){ synced = 1; break; }
    clock_sync_end(synced);
    esp_netif_sntp_deinit();
    s_clock_synced = synced;

    time_t now=0; time(&now);
    struct tm lt; localtime_r(&now,&lt);
    char zone[40]; snprintf(zone,sizeof zone,"%s", clock_tz_posix());
    if(synced)
        ESP_LOGI(TAG,"SNTP ok: clock moved %+lld s -> %04d-%02d-%02d %02d:%02d:%02d local "
                     "(TZ=%s%s)", (long long)(now-before),
                 lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec,
                 zone, clock_tz_known() ? "" : " -- ZONE NOT RECOGNISED");
    else
        ESP_LOGW(TAG,"SNTP did NOT sync in 30 s -- the clock is whatever the last "
                     "checkpoint said (%04d-%02d-%02d %02d:%02d local). Check that UDP 123 "
                     "leaves this network.",
                 lt.tm_year+1900, lt.tm_mon+1, lt.tm_mday, lt.tm_hour, lt.tm_min);

    struct tm ti; gmtime_r(&now,&ti);
    if((ti.tm_year+1900) < 2024) return 0;
    if(synced) clock_checkpoint();   /* real time -> persist it durably (survives power-off) */
    return 1;
}

static char *abspath(char *href, DavCtx *d){
    if(strncmp(href,"http",4)==0){ char *p=strstr(href,"://");
        if(p){ char *s=strchr(p+3,'/'); if(s){ int n=(int)(s-href);
            if(n<(int)sizeof d->base){ memcpy(d->base,href,n); d->base[n]=0; } return s; } } }
    return href;
}

/* ---- News feeds (RSS reader, roadmap #4 stage C) ----------------------------
 * After the PIM sync (Wi-Fi still up), stream each ENABLED feed to SD, parse it
 * in a sliding window, and rebuild the news store. Bounded RAM: the fetch spools
 * to SD (dav_fetch_url), the parser keeps only one item, news_add writes to SD.
 * Feed sources come from the on-SD feed list (bridge/feeds.c), edited in
 * Preferences > News feeds. No credentials -- feeds are public. Compile-verified
 * here; runtime-verified on device. */
#define NEWS_TMP        "/sdcard/.rsstmp"
/* The store used to stop at 30 articles across 15 per feed, which with ten feeds
 * meant most sources never got a look in. Nothing here is held in RAM -- the
 * fetch spools to SD, the parser keeps one item, and the reader seeks one record
 * at a time -- so the cap is really about SD space and sync time, not memory:
 * 240 articles is ~41 KB of index and a few hundred KB of text. */
#define NEWS_MAX_TOTAL  240     /* whole store cap */
#define NEWS_MAX_FEED   40      /* per-feed cap */

/* KEEP TODAY'S NEWS, DROP THE REST. Feeds carry a week of history and the reader
 * shows one headline at a time, so old stories are just distance between the
 * user and today. "Today" is the local calendar date OR anything from the last
 * 24 hours -- the second half matters because a strict calendar test would leave
 * the app nearly empty at 6am, when the overnight stories are technically
 * yesterday's. An item whose date the feed did not give (when == 0) is KEPT: we
 * cannot prove it is old, and dropping it would silently lose whole feeds whose
 * date format we failed to parse. */
#define NEWS_MAX_AGE_S  (24 * 3600)

static int news_is_current(uint32_t when, time_t now){
    if(!when) return 1;                            /* undated -> never assumed old */
    if(when > (uint32_t)now + 3600) return 1;      /* clock skew: a "future" item is fresh */
    if((time_t)when + NEWS_MAX_AGE_S >= now) return 1;
    struct tm a, b;
    time_t w = (time_t)when;
    localtime_r(&w, &a);
    localtime_r(&now, &b);
    return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
}
static int s_news_added;
static int s_news_feeds_ok, s_news_feeds_tried;   /* for the final status line */
static char s_news_why[48];                       /* first failure, verbatim   */
static int s_news_stale;                          /* dropped as older than today */
static time_t s_news_now;                         /* sampled once per run */
static void news_item_cb(const char *title, const char *text, uint32_t when, void *ctx){
    if(s_news_added >= NEWS_MAX_TOTAL) return;
    if(!news_is_current(when, s_news_now)){ s_news_stale++; return; }
    if(news_add((const char*)ctx, title, text, when)) s_news_added++;
}
static void fetch_news(void){
    s_news_added = s_news_feeds_ok = s_news_feeds_tried = s_news_stale = 0;
    s_news_why[0] = 0;
    time(&s_news_now);
    if(feeds_enabled_count() == 0){
        snprintf(s_news_why,sizeof s_news_why,"no feeds enabled");
        ESP_LOGW(TAG,"news: no feeds enabled");
        return;
    }
    setst("Fetching news...");
    /* news_begin() TRUNCATES the store, so a run where every feed fails leaves the
     * reader empty rather than stale. That is the right trade, but it means the
     * status line has to say why -- an empty News app is otherwise unexplained. */
    if(!news_begin()){
        snprintf(s_news_why,sizeof s_news_why,"SD store would not open");
        ESP_LOGE(TAG,"news: store open failed (%s / %s)","/sdcard/news.idx","/sdcard/news.dat");
        return;
    }
    int nf = feeds_count();
    for(int i=0; i<nf && s_news_added<NEWS_MAX_TOTAL; i++){
        const Feed *f = feeds_get(i);
        if(!f || !f->enabled || !f->url[0]) continue;
        s_news_feeds_tried++;
        int before = s_news_added;
        /* the store's two file caches (8 KB) are dead weight during the fetch --
         * hand them back for the length of the handshake, take them again to parse */
        news_suspend();
        int st = dav_fetch_url(f->url, NEWS_TMP);
        long spooled = 0;
        FILE *sp = fopen(NEWS_TMP,"rb");
        if(sp){ fseek(sp,0,SEEK_END); spooled = ftell(sp); fclose(sp); }
        if(st>=200 && st<300 && news_resume()){
            char feed[NEWS_FEED_CAP];
            /* explicit precision: the name (FEED_NAME_CAP) can exceed feed's cap,
             * so bound the copy so the compiler can prove no truncation UB. */
            snprintf(feed, sizeof feed, "%.*s", (int)sizeof feed - 1,
                     f->name[0] ? f->name : "News");
            int got = rss_parse_file(NEWS_TMP, NEWS_MAX_FEED, news_item_cb, (void*)feed);
            if(got > 0) s_news_feeds_ok++;
            else if(!s_news_why[0])
                snprintf(s_news_why,sizeof s_news_why,"%.20s: %ld bytes, no items",
                         f->name, spooled);
            ESP_LOGI(TAG,"news: %s st=%d spooled=%ld parsed=%d kept=%d",
                     f->name, st, spooled, got, s_news_added - before);
        } else {
            /* st < 0 means the request never completed (DNS, TCP or TLS); a
             * positive status means the server answered and refused. Those are
             * different problems and used to print identically. */
            if(!s_news_why[0]){
                if(st < 0) snprintf(s_news_why,sizeof s_news_why,"%.30s unreachable", f->name);
                else       snprintf(s_news_why,sizeof s_news_why,"%.28s HTTP %d", f->name, st);
            }
            ESP_LOGW(TAG,"news: feed '%s' GET st=%d spooled=%ld url=%s",
                     f->name, st, spooled, f->url);
        }
        remove(NEWS_TMP);
    }
    news_resume();                                     /* so commit can patch the count */
    news_commit();
    dav_disconnect();                                  /* free the feed TLS handle */
    ESP_LOGI(TAG,"news: %d items from %d/%d feeds (%d dropped as older than today)%s%s",
             s_news_added, s_news_feeds_ok, s_news_feeds_tried, s_news_stale,
             s_news_why[0] ? " -- " : "", s_news_why);
}

/* ---- weather (Open-Meteo) --------------------------------------------------
 * The last internet stage, and the cheapest: two GETs of ~1.5 KB each, spooled
 * to SD and parsed by bridge/wxfetch.c the same way a feed is. No JSON parser
 * and no new dependency -- Open-Meteo will return CSV if asked, which is the
 * whole reason this fits. No API key, no account. Location comes from
 * config.ini (PRODUCT_PLAN: lat/lon for v1); with none set, this does nothing
 * and says so, because a dashboard quietly showing sample weather forever is
 * how we got here. */
#define WX_TMP "/sdcard/.wxtmp"
static char s_wx_why[48];
static int  s_wx_ok;

static void fetch_weather(const Config *cfg){
    s_wx_ok = 0; s_wx_why[0] = 0;
    char url[512];
    if(!wx_build_url(url, sizeof url, cfg->latitude, cfg->longitude)){
        snprintf(s_wx_why,sizeof s_wx_why,"%s",
                 cfg->latitude[0] || cfg->longitude[0] ? "location not a number" : "no location set");
        ESP_LOGW(TAG,"weather: %s (latitude/longitude in config.ini)", s_wx_why);
        return;
    }
    setst("Fetching weather...");

    int st = dav_fetch_url(url, WX_TMP);
    if(st < 200 || st >= 300){
        snprintf(s_wx_why,sizeof s_wx_why, st < 0 ? "unreachable" : "HTTP %d", st);
        ESP_LOGW(TAG,"weather: forecast GET st=%d", st);
        remove(WX_TMP);
        return;
    }

    WxCache w;
    time_t now = 0; time(&now);
    int ok = wx_parse_file(WX_TMP, (int64_t)now, &w);
    remove(WX_TMP);
    if(!ok){
        snprintf(s_wx_why,sizeof s_wx_why,"reply not understood");
        ESP_LOGW(TAG,"weather: the forecast CSV did not parse");
        return;
    }

    /* AQI is a separate endpoint and a nice-to-have: its failure must not cost
     * us the forecast we already have in hand. */
    if(wx_build_aqi_url(url, sizeof url, cfg->latitude, cfg->longitude)){
        int ast = dav_fetch_url(url, WX_TMP);
        if(ast >= 200 && ast < 300) w.aqi = (int16_t)wx_parse_aqi_file(WX_TMP);
        else ESP_LOGW(TAG,"weather: AQI GET st=%d (forecast kept)", ast);
        remove(WX_TMP);
    }

    FILE *f = fopen(WX_PATH, "wb");
    if(!f){
        snprintf(s_wx_why,sizeof s_wx_why,"could not write %s", WX_PATH);
        ESP_LOGE(TAG,"weather: cannot open %s", WX_PATH);
        return;
    }
    size_t n = fwrite(&w, 1, sizeof w, f);
    fclose(f);
    if(n != sizeof w){
        snprintf(s_wx_why,sizeof s_wx_why,"short write");
        ESP_LOGE(TAG,"weather: short write to %s (%u of %u)", WX_PATH,
                 (unsigned)n, (unsigned)sizeof w);
        return;
    }
    s_wx_ok = 1;
    ESP_LOGI(TAG,"weather: %dF code=%d aqi=%d, %d hourly, sun %02d:%02d-%02d:%02d",
             w.cur_tempF, w.cur_code, w.aqi, w.nhours,
             w.sunrise_min/60, w.sunrise_min%60, w.sunset_min/60, w.sunset_min%60);
}

static void hotsync_task(void *arg){
    (void)arg;
    /* crank up transport logging so a connect failure names its cause */
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_VERBOSE);
    esp_log_level_set("mbedtls", ESP_LOG_VERBOSE);
    esp_log_level_set("esp_http_client", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);

    setst("Connecting Wi-Fi...");
    if(!wifi_up()){ setst("Wi-Fi failed"); wifi_down(); s_busy=0; vTaskDelete(NULL); return; }
    hs_heap("wifi-up");    /* Mode B baseline: Wi-Fi+lwIP paid for, no TLS yet */
    net_probe();           /* lease + DNS, before anything can blame the server */
    setst("Setting clock...");
    /* This one IS fatal, but not because of the account: TLS certificate validity
     * is checked against the wall clock, so an unset clock breaks every fetch
     * below, news included. clock_ok() only fails when the clock is still
     * implausible AFTER the attempt -- a transient SNTP timeout on a device whose
     * clock is already good passes straight through. */
    if(!clock_ok()){ setst("Clock unset - nothing can sync"); wifi_down(); s_busy=0; vTaskDelete(NULL); return; }

    const Config *cfg = appcfg();
    ESP_LOGI(TAG,"config source: %s", appcfg_from_sd() ? "/sdcard/config.ini" : "secrets.h (no config.ini)");

    DavCtx d; memset(&d,0,sizeof d);
    snprintf(d.base,sizeof d.base,"%s",cfg->dav_base);
    snprintf(d.user,sizeof d.user,"%s",cfg->dav_user);
    snprintf(d.pass,sizeof d.pass,"%s",cfg->dav_pass);
    (void)abspath;
    char msg[208];

    /* ---- what is account work, and what is merely internet ----------------
     * The clock above and the news below need nothing but a network. The account
     * stages -- host resolve, login, collections -- need iCloud credentials, and
     * a device with none, or with wrong ones, must still come away with a
     * corrected clock and fresh feeds. So a failure here SKIPS the rest of the
     * account work; it no longer ends the sync. `dav_why` carries the reason
     * into the final status line, because a silent skip reads as a silent
     * success and would hide a bad password indefinitely. */
    /* ntgt also sizes the progress bar: each configured collection owns a band
     * [k/M], the per-app step being the coarse indicator (a finer intra-collection
     * bar would need a callback through sync_collection). Zero of them is a
     * skip reason in its own right -- there is nothing to log in to for. */
    int ntgt=0; for(int i=0;i<N_APPS;i++){ const char*c=app_coll(cfg,i); if(c&&c[0]) ntgt++; }
    int dav_ok = 0;
    char dav_why[48] = "";

    if(!cfg->dav_base[0] || !cfg->dav_user[0] || !cfg->dav_pass[0]){
        snprintf(dav_why,sizeof dav_why,"no account set up");
        ESP_LOGW(TAG,"no iCloud credentials; internet-only sync");
    } else if(!ntgt){
        snprintf(dav_why,sizeof dav_why,"no collections chosen");
        ESP_LOGW(TAG,"no collections configured; internet-only sync");
    } else {
        hs_heap("pre-tls");    /* last sample before the first HTTPS request */
        setst("Resolving host...");
        char host[256]="";
        if(dav_effective_host(&d,"/",host,sizeof host)==0 && host[0]){
            snprintf(d.base,sizeof d.base,"%s",host);
            ESP_LOGI(TAG,"effective host: %s",host);
        } else {
            ESP_LOGW(TAG,"host resolve failed (HTTP %d); using %s",dav_last_status,d.base);
        }

        /* probe login first so we can tell an auth failure from a bad collection */
        setst("Checking login...");
        char principal[256]="";
        if(dav_prop_href(&d,"/","<d:current-user-principal/>","",principal,sizeof principal)!=0 || !principal[0]){
            /* A rejected password and an unreachable server are different faults
             * and used to print the same line ("login failed (HTTP 0)"), which
             * sent us looking at the password when the request never left. */
            if(dav_last_status == 401 || dav_last_status == 403)
                snprintf(dav_why,sizeof dav_why,"password rejected");
            else if(dav_last_status == 0)
                snprintf(dav_why,sizeof dav_why,"server unreachable");
            else
                snprintf(dav_why,sizeof dav_why,"login failed (HTTP %d)",dav_last_status);
            ESP_LOGE(TAG,"iCloud: %s (HTTP %d)",dav_why,dav_last_status);
            dav_disconnect();          /* drop the dead TLS handle before the news fetch */
        } else {
            ESP_LOGI(TAG,"principal: %s",principal);
            dav_ok = 1;
        }
        /* THE number: a full handshake has completed, so min_ever now carries its peak.
         * Compare min_ever here against the "pre-tls" sample -- the drop is the cost of
         * mbedTLS with the whole UI still resident. */
        hs_heap("post-tls");
    }

    /* Address book (CardDAV) lives on a separate iCloud host; resolve it lazily
     * the first time a card target is reached. Same credentials as caldav. */
    DavCtx dcard; int card_ready=0;

    SyncStats tot={0};
    int did=0, failed=0, protec=0;
    ConflictPolicy pol = (ConflictPolicy)cfg->policy;
    int step=0;
    setprog(0);
    sync_set_progress(hs_prog_cb, NULL);           /* engine drives the intra-collection bar */
    for(int i=0; dav_ok && i<N_APPS; i++){
        const SyncApp *t=&s_apps[i];
        const char *coll=app_coll(cfg,i);
        if(!coll || !coll[0]) continue;            /* app not configured -> skip */
        step++;
        /* this collection fills the band [(step-1)/ntgt .. step/ntgt] of the bar */
        s_band_lo   = ntgt ? (step-1)*100/ntgt : 0;
        s_band_span = ntgt ? 100/ntgt : 100;
        setprog(s_band_lo);

        DavCtx *ctx=&d;
        if(t->card){
            if(!card_ready){
                memset(&dcard,0,sizeof dcard);
                snprintf(dcard.base,sizeof dcard.base,"%s",cfg->dav_card_base);
                snprintf(dcard.user,sizeof dcard.user,"%s",cfg->dav_user);
                snprintf(dcard.pass,sizeof dcard.pass,"%s",cfg->dav_pass);
                char chost[256]="";
                if(dav_effective_host(&dcard,"/",chost,sizeof chost)==0 && chost[0]){
                    snprintf(dcard.base,sizeof dcard.base,"%s",chost);
                    ESP_LOGI(TAG,"contacts host: %s",chost);
                } else ESP_LOGW(TAG,"contacts host: no principal href in the reply "
                                    "(HTTP %d); using %s",dav_last_status,dcard.base);
                card_ready=1;
            }
            ctx=&dcard;
        }

        snprintf(msg,sizeof msg,"[%d/%d] Syncing %s...",step,ntgt,t->name); setst(msg);
        ESP_LOGI(TAG,"sync %s coll=%s pdb=%s heap=%lu largest=%lu",t->name,coll,t->pdb,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        SyncStats st={0};
        int n = sync_collection(ctx, t->pdb, t->pdb, coll, t->kind, t->map, pol, &st);
        /* n == -2: guard refused to overwrite a non-empty PDB with an empty result
         * (data was protected). n == -1: local out-of-memory. n >= 0: records kept. */
        ESP_LOGI(TAG,"%s: rc=%d up +%d~%d-%d down +%d~%d-%d heap=%lu",t->name,n,
                 st.pushNew,st.pushMod,st.pushDel, st.pullNew,st.pullMod,st.pullDel,
                 (unsigned long)esp_get_free_heap_size());
        if(n == -2) protec++;
        else if(n < 0) failed++;
        else { did++;
            tot.pushNew+=st.pushNew; tot.pushMod+=st.pushMod; tot.pushDel+=st.pushDel;
            tot.pullNew+=st.pullNew; tot.pullMod+=st.pullMod; tot.pullDel+=st.pullDel; }
    }

    sync_set_progress(NULL, NULL);                 /* detach the hook */
    sync_free_scratch();   /* hand the ~20 KB sync scratch back to the interactive UI */
    /* The account phase leaves a keep-alive TLS session open and the heap
     * fragmented; the news phase is ten short-lived handshakes that each need a
     * contiguous block. Measured: largest8 fell 27 KB -> 17 KB -> 10 KB across the
     * account work and feeds began failing at FETCH_HEADER. Drop the session
     * explicitly so the feeds start from whatever the heap can actually give back. */
    dav_disconnect();
    hs_heap("pre-news");
    fetch_news();          /* RSS reader: fetch configured feeds while Wi-Fi is up */
    fetch_weather(cfg);    /* lock-screen dashboard: the last thing the network is for */
    dav_disconnect();
    setprog(100);
    /* Every internet stage reports its own outcome. The old line asserted "Clock +
     * news done" whether or not either had happened, so a run that set nothing and
     * fetched nothing still read as a success with a credentials footnote. */
    char clk[24], nws[64], wxs[40];
    snprintf(wxs,sizeof wxs, s_wx_ok ? "; weather" : "; no weather (%.24s)",
             s_wx_why[0] ? s_wx_why : "failed");
    snprintf(clk,sizeof clk,"%s", s_clock_synced ? "Clock set" : "CLOCK NOT SYNCED");
    if(s_news_added > 0)
        snprintf(nws,sizeof nws,"%d articles", s_news_added);
    else
        snprintf(nws,sizeof nws,"no news (%.47s)", s_news_why[0] ? s_news_why : "all feeds failed");

    if(!dav_ok)
        snprintf(msg,sizeof msg,"%.23s; %.63s%.39s; no records - %.39s",clk,nws,wxs,dav_why);
    else if(did==0 && failed>0)
        snprintf(msg,sizeof msg,"Sync failed - low memory (heap %lu)",
                 (unsigned long)esp_get_free_heap_size());
    else
        snprintf(msg,sizeof msg,"Done: +%d~%d-%d up +%d~%d-%d down%.16s%.20s%.10s",
                 tot.pushNew,tot.pushMod,tot.pushDel, tot.pullNew,tot.pullMod,tot.pullDel,
                 (failed||protec)?" (some skipped)":"",
                 s_clock_synced ? "" : " - CLOCK NOT SYNCED",
                 s_news_added ? "" : " - no news");
    ESP_LOGI(TAG,"%s",msg);
    setst(msg);
    hs_heap("sync-done");  /* after sync_free_scratch(), before Wi-Fi goes away */
    wifi_down();           /* also closes the last collection's keep-alive connection */
    /* Back in Mode A. If largestDMA here is much smaller than it was at "wifi-up",
     * a Wi-Fi cycle fragments the DMA region -- which is what would break a plan to
     * bring Wi-Fi up before LVGL allocates its contiguous draw buffer. */
    hs_heap("wifi-down");
    /* HOW BIG SHOULD THIS TASK'S STACK BE? Its stack is heap, and on this device
     * heap is exactly what the mbedTLS handshake ran out of -- so an over-sized
     * stack is not free caution, it is the thing that breaks the sync. This is
     * the measurement that lets HOTSYNC_STACK be a number rather than a guess:
     * the high-water mark is the smallest the stack ever got, in bytes. */
    ESP_LOGI(TAG,"stack: %u bytes of %u still free at the end",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)HOTSYNC_STACK);
    s_busy = 0;
    vTaskDelete(NULL);
}

/* ---- collection discovery (Preferences) ----------------------------------
 * Same background-task slot as the sync (guarded by s_busy). Results land in a
 * small fixed array; the UI reads them after hotsync_discover_done(). */
#define MAX_DISC 24
static DiscColl s_disc[MAX_DISC];
static volatile int s_disc_n;
static volatile int s_disc_done;

int hotsync_discover_busy(void){ return s_busy; }
int hotsync_discover_done(void){ return s_disc_done; }
int hotsync_discover_count(void){ return s_disc_n; }
const DiscColl *hotsync_discover_get(int i){ return (i>=0 && i<s_disc_n) ? &s_disc[i] : NULL; }

/* dav_list_collections callback: keep calendars + address books, normalise the
 * href to the "no leading/trailing slash" form sync expects (secrets.h note). */
static void disc_add(const char *href, int kind, const char *dn, void *ctx){
    (void)ctx;
    if((kind!='c' && kind!='a') || !href || !href[0]) return;
    if(s_disc_n >= MAX_DISC){ ESP_LOGW(TAG,"discovery: more than %d collections; list truncated",MAX_DISC); return; }
    const char *s = href; while(*s=='/') s++;
    char tmp[192]; snprintf(tmp,sizeof tmp,"%s",s);
    size_t n = strlen(tmp); while(n && tmp[n-1]=='/') tmp[--n]=0;
    if(!tmp[0]) return;
    DiscColl *e = &s_disc[s_disc_n++];
    snprintf(e->href,sizeof e->href,"%s",tmp);
    snprintf(e->name,sizeof e->name,"%s",(dn&&dn[0])?dn:"(unnamed)");
    e->kind = kind;
    ESP_LOGI(TAG,"discovered [%c] %s (%s)", (char)kind, e->name, e->href);
}

/* PROPFIND `prop` at `path` (which itself may be an absolute URL); retarget
 * d->base if either the request path or the returned href is a full URL, and
 * hand back the host-relative path portion. 0 on success, -1 on failure. */
static int disc_prop(DavCtx *d, const char *path, const char *prop,
                     const char *ns, char *out, int cap){
    char pbuf[512]; snprintf(pbuf,sizeof pbuf,"%s",(path&&path[0])?path:"/");
    char *rel = abspath(pbuf, d);
    char href[512]="";
    if(dav_prop_href(d, rel, prop, ns, href, sizeof href)!=0 || !href[0]) return -1;
    char *hp = abspath(href, d);            /* retargets d->base when href is a URL */
    snprintf(out, cap, "%s", hp);
    return 0;
}

static void discover_task(void *arg){
    (void)arg;
    s_disc_n = 0; s_disc_done = 0;
    setst("Connecting Wi-Fi...");
    if(!wifi_up()){ setst("Wi-Fi failed"); s_disc_done=1; wifi_down(); s_busy=0; vTaskDelete(NULL); return; }
    hs_heap("disc wifi-up");   /* second Mode B data point: discovery also does TLS */
    const Config *cfg = appcfg();

    /* --- CalDAV: calendars + reminders lists --- */
    DavCtx d; memset(&d,0,sizeof d);
    snprintf(d.base,sizeof d.base,"%s",cfg->dav_base);
    snprintf(d.user,sizeof d.user,"%s",cfg->dav_user);
    snprintf(d.pass,sizeof d.pass,"%s",cfg->dav_pass);
    char host[256]="";
    if(dav_effective_host(&d,"/",host,sizeof host)==0 && host[0]) snprintf(d.base,sizeof d.base,"%s",host);
    setst("Finding calendars...");
    char principal[512]="";
    if(disc_prop(&d,"/","<d:current-user-principal/>","",principal,sizeof principal)==0){
        char home[512]="";
        if(disc_prop(&d,principal,"<c:calendar-home-set/>",
                     "xmlns:c=\"urn:ietf:params:xml:ns:caldav\"",home,sizeof home)==0){
            ESP_LOGI(TAG,"calendar-home: %s",home);
            dav_list_collections(&d,home,disc_add,NULL);
        } else ESP_LOGW(TAG,"calendar-home-set failed (HTTP %d)",dav_last_status);
    } else ESP_LOGW(TAG,"caldav principal failed (HTTP %d)",dav_last_status);
    int ncal = s_disc_n;

    /* --- CardDAV: address books (separate iCloud host) --- */
    setst("Finding address books...");
    DavCtx dc; memset(&dc,0,sizeof dc);
    snprintf(dc.base,sizeof dc.base,"%s",cfg->dav_card_base);
    snprintf(dc.user,sizeof dc.user,"%s",cfg->dav_user);
    snprintf(dc.pass,sizeof dc.pass,"%s",cfg->dav_pass);
    char chost[256]="";
    if(dav_effective_host(&dc,"/",chost,sizeof chost)==0 && chost[0]) snprintf(dc.base,sizeof dc.base,"%s",chost);
    char cprin[512]="";
    if(disc_prop(&dc,"/","<d:current-user-principal/>","",cprin,sizeof cprin)==0){
        char chome[512]="";
        if(disc_prop(&dc,cprin,"<c:addressbook-home-set/>",
                     "xmlns:c=\"urn:ietf:params:xml:ns:carddav\"",chome,sizeof chome)==0){
            ESP_LOGI(TAG,"addressbook-home: %s",chome);
            dav_list_collections(&dc,chome,disc_add,NULL);
        } else ESP_LOGW(TAG,"addressbook-home-set failed (HTTP %d)",dav_last_status);
    } else ESP_LOGW(TAG,"carddav principal failed (HTTP %d)",dav_last_status);

    hs_heap("disc post-tls");  /* both hosts handshaked by here */
    s_disc_done = 1;
    if(s_disc_n==0) setst("No collections found - check login");
    else { char m[80]; snprintf(m,sizeof m,"Found %d (%d cal, %d addr)",s_disc_n,ncal,s_disc_n-ncal); setst(m); }
    wifi_down();           /* also closes the discovery keep-alive connection */
    s_busy = 0;
    vTaskDelete(NULL);
}

void hotsync_discover_start(void){
    if(s_busy) return;
    s_busy = 1; s_disc_done = 0; s_disc_n = 0;
    setst("Starting...");
    if(xTaskCreate(discover_task, "discover", HOTSYNC_STACK, NULL, 4, NULL) != pdPASS){
        setst("Could not start discovery"); s_disc_done=1; s_busy = 0;
    }
}

void hotsync_start(void){
    if(s_busy) return;
    s_busy = 1;
    setst("Starting...");
    /* 32 KB stack. The task stack is malloc'd from the DRAM heap, so an overflow
     * corrupts adjacent heap metadata -> a later alloc crashes deep in tlsf
     * (block_locate_free) rather than a clean stack-canary trip. The streaming
     * reconcile's merge loop calls DAV ops (each a full mbedTLS handshake, whose
     * stack spikes past the ~8 KB estimate on iCloud's cert chain) from a frame
     * that also holds the row structs + line buffers, and resolveServer nests a
     * GET per relocated object -- 20 KB overflowed at ~30 records. The old worry
     * that 32 KB starves the heap no longer holds: streaming shrank the S struct
     * from ~16 KB to ~3 KB, so the contiguous block it needs is tiny now. */
    if(xTaskCreate(hotsync_task, "hotsync", HOTSYNC_STACK, NULL, 4, NULL) != pdPASS){
        setst("Could not start sync task"); s_busy = 0;
    }
}
