/* hotsync.c -- HotSync: Wi-Fi up, set the clock, fetch the news, place the
 * device, fetch the weather, Wi-Fi down.
 *
 * Runs on its own task with a live status string the UI polls. DEFENSIVE: no
 * ESP_ERROR_CHECK -- every step checks its return and fails to a status
 * message, so a network/RAM problem can't crash the interactive UI. Wi-Fi is
 * brought up only for the run, beside the live UI; the heap it leaves is
 * logged at every phase (hs_heap_check).
 *
 * Until 2026-11-07 it also synced Date Book, To Do and Address with iCloud
 * (CalDAV/CardDAV). That was archived with those apps (git tag
 * archive/pim-caldav); what is left needs a network and no account.
 */
#include "hotsync.h"
#include "safefile.h"     /* weather.dat is replaced whole */
#include "dav.h"          /* the HTTPS transport the fetches use */
#include "mbedtls/platform_util.h"   /* mbedtls_platform_zeroize: the Wi-Fi password */
#include "rss.h"          /* RSS reader: feed parser */
#include "news.h"         /* RSS reader: on-SD article store */
#include "feeds.h"        /* RSS reader: the enabled feed sources */
#include "dash.h"         /* lock-screen dashboard: the WxCache it renders from */
#include "wxfetch.h"      /* Open-Meteo CSV -> WxCache */
#include "appcfg.h"
#include "geoip.h"
#include "clock.h"
#include "power.h"        /* drain log: a sync is the expensive interval */
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

/* The task's stack comes out of the same heap the TLS handshake needs. It was
 * 32 KB for the iCloud sync engine, whose record walk put a 4 KB record buffer
 * on it per record; an overflow did not fault but wrote into the neighbouring
 * heap blocks, and cost days. The engine is gone (archived 2026-11-07), and
 * the fetches left are lighter, but the size stays until a completed run's
 * high-water log (hs_heap_check, the "stack:" line at the end) says what it
 * can be cut to. Change it only against THAT log. */
#define HOTSYNC_STACK 32768
static volatile int s_busy;
/* Cancellation. Both are plain ints written by one side and read by the other,
 * which is safe here: 32-bit aligned loads/stores are atomic on this core, and
 * neither is part of a read-modify-write. s_cancel is raised by the LVGL task
 * and cleared by the sync task; s_was_cancelled survives the run so the UI can
 * report how it ended. */
static volatile int s_cancel;
static volatile int s_was_cancelled;
static char s_status[208] = "Ready";
static volatile int s_wifi_problem;       /* HS_WIFI_*, for the last run */
int hotsync_wifi_problem(void){ return s_wifi_problem; }
static void setst(const char *s){ snprintf(s_status, sizeof s_status, "%s", s); }

void hotsync_cancel(void){ if(s_busy) s_cancel = 1; }
int  hotsync_cancel_pending(void){ return s_busy && s_cancel; }
int  hotsync_cancelled(void){ return s_was_cancelled; }

/* The one place the sync task asks "should I stop?". Called only where stopping
 * leaves the card consistent -- between collections and between stages. */
static int hs_stop(void){ return s_cancel; }

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
/* TEMPORARY (memory rework): walk the whole heap and shout at the first phase
 * whose blocks no longer make sense. A corrupted heap does not fail where it was
 * corrupted -- on this device it surfaced in lwIP's RX path and in our own
 * largest-free-block logging -- so the only way to find the culprit is to check
 * at known points and see which one is the first to fail. */
/* ---- per-phase guard rails -------------------------------------------------
 * This hook exists because of a bug that cost days. The sync task's stack is a
 * heap allocation, so overflowing it does not fault -- it writes into the next
 * heap block, and the damage only surfaces much later, somewhere unrelated. It
 * showed up as "out of memory", then as a crash inside lwIP's receive path, then
 * inside our own largest-free-block logging. None of those were the bug.
 *
 * The STACK check is cheap (one read of a FreeRTOS counter) and stays on: it
 * reports the high-water mark at each phase and shouts before an overflow rather
 * than after. That is what turns this class of bug into a one-line log.
 *
 * The HEAP walk is expensive -- it visits every block -- so it is compiled out
 * unless something is actually suspected. Build with -DHS_DEEP_HEAP_CHECK=1
 * (ideally alongside CONFIG_HEAP_POISONING_COMPREHENSIVE) to bring it back. */
#ifndef HS_DEEP_HEAP_CHECK
#define HS_DEEP_HEAP_CHECK 0
#endif
#define HS_STACK_FLOOR 4096          /* shout below this many bytes free */

static unsigned s_stack_lo = 0xFFFFFFFFu;
static int s_stack_warned;
#if HS_DEEP_HEAP_CHECK
static int s_heap_bad;
#endif
static void hs_heap_check(const char *where){
    unsigned sfree = (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
    if(sfree < s_stack_lo){
        s_stack_lo = sfree;
        ESP_LOGD(TAG,"stack low-water %u of %u (used %u) at '%s'",
                 sfree, (unsigned)HOTSYNC_STACK, (unsigned)HOTSYNC_STACK - sfree, where);
    }
    if(sfree < HS_STACK_FLOOR && !s_stack_warned){
        s_stack_warned = 1;
        ESP_LOGE(TAG,"STACK NEARLY OUT at '%s': %u of %u free. Raise HOTSYNC_STACK -- "
                     "an overflow here corrupts the heap silently, it does not crash here.",
                 where, sfree, (unsigned)HOTSYNC_STACK);
    }
#if HS_DEEP_HEAP_CHECK
    if(s_heap_bad) return;
    if(heap_caps_check_integrity_all(false)) return;   /* printing the detail trips the WDT */
    s_heap_bad = 1;
    ESP_LOGE(TAG,"HEAP CORRUPT first detected at checkpoint '%s'", where);
#endif
}

static void hs_heap(const char *where){
    ESP_LOGI(TAG,"heap[%s]: free=%lu min_ever=%lu largest8=%lu largestDMA=%lu",
             where,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

/* progress 0..100 for the UI status line, by stage; -1 = idle/not started */
static volatile int s_prog = -1;
static void setprog(int p){ s_prog = p; }
int hotsync_progress(void){ return s_prog; }

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
    /* STA_START does NOT auto-connect any more. With one remembered network the
     * config was already set before esp_wifi_start(), so connecting on START was
     * the same thing as connecting on purpose. With four, the radio starts with
     * no network chosen and wifi_try() sets each one and connects it explicitly
     * -- an auto-connect here would fire against an empty config and spend the
     * first network's retry budget failing to join "".
     *
     * Three retries, not eight: eight was a single network's whole budget, and
     * four networks at eight retries each is over a minute of a user watching a
     * status line. An AP in range associates in seconds. */
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED){
        if(s_retries++ < 3) esp_wifi_connect();
        else xEventGroupSetBits(s_evt, WIFI_FAIL);
    } else if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP){
        s_retries=0; xEventGroupSetBits(s_evt, WIFI_OK);
    }
}

/* Try ONE remembered network. The radio is already initialised and started by
 * wifi_up(); this just points it at a different AP and waits. */
static int wifi_try(int slot, int timeout_ms){
    const WifiNet *n = &appcfg()->wifi[slot];
    wifi_config_t wc = { 0 };
    strncpy((char*)wc.sta.ssid, n->ssid, sizeof wc.sta.ssid);
    /* The password comes out of the store for exactly as long as it takes the
     * driver to copy it (the driver keeps it in RAM only: WIFI_STORAGE_RAM). */
    appcfg_wifi_pass(slot, (char*)wc.sta.password, sizeof wc.sta.password);
    s_retries = 0;
    xEventGroupClearBits(s_evt, WIFI_OK|WIFI_FAIL);
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &wc);
    mbedtls_platform_zeroize(&wc, sizeof wc);
    if(e!=ESP_OK) return 0;
    /* Never log a password, and never log the SSID's contents beyond this: the
     * status line is shown on screen and the log goes to a shared console. */
    ESP_LOGI(TAG, "wifi: trying \"%s\"", n->ssid);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_evt, WIFI_OK|WIFI_FAIL, pdFALSE, pdFALSE,
                                        pdMS_TO_TICKS(timeout_ms));
    return (b & WIFI_OK) ? 1 : 0;
}

/* The radio up and started, associated with NOTHING. A scan needs exactly this
 * and no more; a sync needs this and then a join. Paired with wifi_down(), so
 * every exit path leaves the gauge and the radio in the same state. */
static int radio_up(void){
    power_busy(1);          /* the gauge must not read a sagging cell as a flat one */
    s_evt = xEventGroupCreate();
    if(esp_netif_init()!=ESP_OK) return 0;
    if(esp_event_loop_create_default()!=ESP_OK){ /* may already exist */ }
    if(!s_netif) s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if(esp_wifi_init(&cfg)!=ESP_OK) return 0;
    /* Keep the driver's copy of the network config (which holds the password)
     * in RAM only. The default also writes it to NVS, which would be a second
     * copy of every password the store already holds, outside the store. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_ev, NULL, &s_h_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_ev, NULL, &s_h_ip);
    if(esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK) return 0;
    if(esp_wifi_start()!=ESP_OK) return 0;
    return 1;
}

static int wifi_up(void){
    if(!radio_up()) return 0;

    /* up to four remembered networks, in the order the config holds them,
     * which is most-recently-connected first. The FIRST one gets the long wait
     * (25 s) because in the overwhelmingly common case you are where you were
     * last time and it is going to work; the rest get a short one, because four
     * full timeouts in a row is over a minute and a half of a user watching a
     * status line, and an AP that is actually in range associates in seconds.
     *
     * A network that connects is promoted to the front and the config is
     * persisted, so the order is self-correcting: move between home and work and
     * the device follows you after one sync each way. */
    const Config *pc = appcfg();
    for(int i = 0; i < CFG_WIFI_N; i++){
        if(!pc->wifi[i].ssid[0]) continue;                  /* empty slot */
        if(s_cancel) break;
        if(!wifi_try(i, i == 0 ? 25000 : 8000)) continue;
        if(config_wifi_promote(appcfg_mut(), i)){
            appcfg_save();                                  /* the order changed */
            ESP_LOGI(TAG, "wifi: promoted slot %d to first", i + 1);
        }
        (void)s_netif_inited;
        return 1;
    }
    (void)s_netif_inited;
    return 0;
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
    power_busy(0);      /* here, not at each exit path: every one of them lands on this */
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

/* ---- News feeds -------------------------------------------------------------
 * With Wi-Fi up and the clock set, stream each ENABLED feed to SD, parse it
 * in a sliding window, and rebuild the news store. Bounded RAM: the fetch spools
 * to SD (dav_fetch_url), the parser keeps only one item, news_add writes to SD.
 * Feed sources come from the on-SD feed list (bridge/feeds.c), edited in
 * Settings > News. No credentials -- feeds are public. */
#define NEWS_TMP        "/sdcard/.rsstmp"
/* Generous enough that ten feeds all get a look in. Nothing here is held in RAM -- the
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
/* TODAY, by the local calendar date -- not "within the last 24 hours".
 *
 * The rolling 24-hour window kept a slice of yesterday alive: at 09:00 it let
 * through everything published after 09:00 the previous day, so the reader was
 * always part yesterday's news. A calendar-day test is what "today's news"
 * actually means, and it is what the store now holds.
 *
 * Two things are still kept. A dated item in the FUTURE is kept, because that is
 * clock skew between us and the publisher, not a stale story. An UNDATED item is
 * kept, because the feed is still listing it and we have no evidence it is old --
 * inventing an age for it would silently empty feeds that publish no dates. */
static int news_is_current(uint32_t when, time_t now){
    if(!when) return 1;                            /* undated -> no evidence it is old */
    if(when > (uint32_t)now + 3600) return 1;      /* clock skew: a "future" item is fresh */
    struct tm a, b;
    time_t w = (time_t)when;
    localtime_r(&w, &a);
    localtime_r(&now, &b);
    return a.tm_year == b.tm_year && a.tm_yday == b.tm_yday;
}
static int s_news_added;
static int s_news_feeds_ok, s_news_feeds_tried;   /* for the final status line */
static char s_news_why[48];                       /* first failure, verbatim   */
static int s_news_stale;                          /* dropped: dated before today */
static int s_news_undated;                        /* kept, but with no date to judge */
static time_t s_news_now;                         /* sampled once per run */
static void news_item_cb(const char *title, const char *text, uint32_t when, void *ctx){
    if(s_news_added >= NEWS_MAX_TOTAL) return;
    if(!news_is_current(when, s_news_now)){ s_news_stale++; return; }
    if(!when) s_news_undated++;
    if(news_add((const char*)ctx, title, text, when)) s_news_added++;
}
static void fetch_news(void){
    s_news_added = s_news_feeds_ok = s_news_feeds_tried = s_news_stale = s_news_undated = 0;
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
             * different problems and must not print identically. */
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
    ESP_LOGI(TAG,"news: %d items from %d/%d feeds (%d dropped as not today, %d kept undated)%s%s",
             s_news_added, s_news_feeds_ok, s_news_feeds_tried, s_news_stale, s_news_undated,
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
#define GEO_TMP "/sdcard/.geotmp"   /* the IP-location reply, one short line */
static char s_wx_why[48];
static int  s_wx_ok;

/* ---- where are we? (the first sync only) ---------------------------------
 * Latitude and longitude were the last two values in Settings that could only be
 * entered as numbers, and a wrong one fails silently: weather simply never
 * appears. A device that is on the internet at all already knows enough to place
 * itself in the right town, which is all a forecast can use.
 *
 * ONLY WHEN THE LOCATION IS UNSET, so this is one request in a device's life
 * rather than one per sync, and a coordinate the user chose is never overwritten
 * by a guess. The timezone comes back in the same reply and is taken on the same
 * terms -- only if the device does not already have one.
 *
 * Runs before fetch_weather() so the forecast in THIS sync uses what it found:
 * the alternative is telling somebody their brand new device will have weather
 * tomorrow. See bridge/geoip.h for why this is IP-based and not Wi-Fi-based, and
 * why it is plain HTTP. */
/* What the location lookup did, for the status line. The first version of this
 * reported only to the serial log, and the first thing that went wrong on a real
 * device was a lookup that never ran -- which looked, on the glass, exactly like
 * a lookup that ran and changed nothing. A sync that silently declines to do a
 * thing has to say so where the person tapping Sync can read it. */
static char s_geo_why[40];

static void locate_by_ip(Config *cfg){
    /* THE TEST IS "IS THIS LOCATION APPROXIMATE", not "is it missing", and the
     * difference is the whole point. Picking a city off a list of two dozen is
     * not "I am in New York", it is "New York is the nearest one you offered
     * me" -- so the person who picked from the list is exactly the one who
     * needs a refined coordinate. An approximate location is re-derived every sync, which also
     * means the weather follows a device that travels; a typed one never is. */
    s_geo_why[0] = 0;
    if(!cfg->loc_auto && cfg->latitude[0] && cfg->longitude[0]){
        snprintf(s_geo_why, sizeof s_geo_why, "location kept as set");
        return;
    }
    setst("Finding your area...");

    int st = dav_fetch_url(geoip_url(), GEO_TMP);
    if(st < 200 || st >= 300){
        snprintf(s_geo_why, sizeof s_geo_why, st < 0 ? "no location (unreachable)"
                                                     : "no location (HTTP %d)", st);
        ESP_LOGW(TAG,"geoip: GET st=%d (location unchanged)", st);
        remove(GEO_TMP);
        return;
    }
    char body[160] = "";
    FILE *f = fopen(GEO_TMP, "rb");
    if(f){ size_t n = fread(body, 1, sizeof body - 1, f); body[n] = 0; fclose(f); }
    remove(GEO_TMP);
    /* LOG THE REPLY VERBATIM. It is a coordinate and a town -- not a credential
     * -- and when this did not work on a real device the one question nobody
     * could answer was "what did the server actually say". A parser that only
     * reports "not understood" leaves you guessing at quoting, field order and
     * captive portals. Trim the newline so the log stays one line. */
    for(char *p = body; *p; p++) if(*p == '\n' || *p == '\r'){ *p = 0; break; }
    ESP_LOGI(TAG,"geoip: HTTP %d, reply: %s", st, body[0] ? body : "(empty)");

    char lat[sizeof cfg->latitude], lon[sizeof cfg->longitude];
    char tz[sizeof cfg->timezone], city[sizeof cfg->loc_name];
    if(!geoip_parse(body, lat, sizeof lat, lon, sizeof lon, tz, sizeof tz, city, sizeof city)){
        /* Whatever was there stays there: a zone-derived coordinate is a worse
         * answer than this one would have been, and a far better answer than
         * none. Two ways this legitimately happens -- a carrier NAT answering
         * "fail,private range", and a captive portal answering HTML. */
        snprintf(s_geo_why, sizeof s_geo_why, "no location (reply not understood)");
        ESP_LOGW(TAG,"geoip: reply not usable (location unchanged)");
        return;
    }
    snprintf(cfg->latitude,  sizeof cfg->latitude,  "%s", lat);
    snprintf(cfg->longitude, sizeof cfg->longitude, "%s", lon);
    snprintf(cfg->loc_name,  sizeof cfg->loc_name,  "%s", city);
    cfg->loc_auto = 1;             /* still approximate: keep improving it */
    /* The zone is a bonus, and it is taken on the same terms: a device that has
     * never been configured has no zone either, and this is the one moment it
     * can learn both. A zone the user picked is left alone. */
    int took_tz = 0;
    if(tz[0] && !cfg->timezone[0]){
        snprintf(cfg->timezone, sizeof cfg->timezone, "%s", tz);
        clock_set_tz(cfg->timezone);
        took_tz = 1;
    }
    appcfg_save();
    /* Coordinates are not secret and this line is the only way to tell a wrong
     * placement from a failed one, which is the whole reason weather is blank. */
    snprintf(s_geo_why, sizeof s_geo_why, "located %.24s",
             cfg->loc_name[0] ? cfg->loc_name : cfg->latitude);
    ESP_LOGI(TAG,"geoip: located at %s,%s (%s)%s%s", cfg->latitude, cfg->longitude,
             cfg->loc_name[0] ? cfg->loc_name : "unnamed",
             took_tz ? " tz=" : "", took_tz ? cfg->timezone : "");
}

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

    SafeFile sf;
    FILE *f = sf_open(&sf, WX_PATH, "wb");
    if(!f){
        snprintf(s_wx_why,sizeof s_wx_why,"could not write %s", WX_PATH);
        ESP_LOGE(TAG,"weather: cannot open %s", WX_PATH);
        return;
    }
    size_t n = fwrite(&w, 1, sizeof w, f);
    if(sf_commit(&sf, n == sizeof w) != 0) n = 0;
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

    /* Count it against the current drain-log interval BEFORE the radio comes up,
     * so a sync that fails at Wi-Fi still shows up as the reason that interval
     * cost more than the one before it. */
    power_note_sync();

    setst("Connecting Wi-Fi...");
    if(!wifi_up()){
        /* Say which failure it was: the UI sends the user to the Wi-Fi panel
         * either way, but "nothing saved" and "nothing answered" need
         * different words there. */
        int saved = 0;
        for(int i = 0; i < CFG_WIFI_N; i++) if(appcfg()->wifi[i].ssid[0]) saved++;
        s_wifi_problem = saved ? HS_WIFI_NO_JOIN : HS_WIFI_NONE_SAVED;
        setst(saved ? "Wi-Fi failed" : "No Wi-Fi network saved");
        wifi_down(); s_busy=0; vTaskDelete(NULL); return;
    }
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
    ESP_LOGI(TAG,"config source: %s", appcfg_from_sd() ? "/sdcard/config.ini" : "defaults (no config.ini)");
    /* Say out loud what the file actually yielded. "no location set" was true but
     * unfalsifiable from here -- it could not distinguish a config.ini without the
     * keys from one whose values failed to parse. Coordinates are not secret. */
    ESP_LOGI(TAG,"location: latitude=%s longitude=%s",
             cfg->latitude[0]  ? cfg->latitude  : "(unset)",
             cfg->longitude[0] ? cfg->longitude : "(unset)");
    char msg[208];
    setprog(10);
    dav_transport_reset();
    dav_set_check(hs_heap_check);
    hs_heap("pre-news");
    /* The safe points. News and weather each replace a cache wholesale, so
     * skipping them on cancel loses nothing that was half-done -- the previous
     * cache simply stays. */
    if(!hs_stop()){
        fetch_news();      /* RSS reader: fetch configured feeds while Wi-Fi is up */
        setprog(60);
    }
    if(!hs_stop()){
        locate_by_ip(appcfg_mut());  /* no-op while the location is pinned */
        setprog(75);
        fetch_weather(cfg);/* lock-screen dashboard: the last thing the network is for */
    }
    dav_disconnect();
    setprog(100);
    /* Every internet stage reports its own outcome. The old line asserted "Clock +
     * news done" whether or not either had happened, so a run that set nothing and
     * fetched nothing still read as a success with a credentials footnote. */
    char clk[24], nws[64], wxs[40], geo[48];
    snprintf(wxs,sizeof wxs, s_wx_ok ? "; weather" : "; no weather (%.24s)",
             s_wx_why[0] ? s_wx_why : "failed");
    snprintf(clk,sizeof clk,"%s", s_clock_synced ? "Clock set" : "CLOCK NOT SYNCED");
    snprintf(geo,sizeof geo, s_geo_why[0] ? "; %.40s" : "%s", s_geo_why[0] ? s_geo_why : "");
    if(s_news_added > 0)
        snprintf(nws,sizeof nws,"%d articles", s_news_added);
    else
        snprintf(nws,sizeof nws,"no news (%.47s)", s_news_why[0] ? s_news_why : "all feeds failed");

    /* The widths are a BUDGET, not decoration: msg is 208 bytes, and ESP-IDF
     * catches an overflow as -Werror=format-truncation where the simulator's
     * plainer -Wall does not. 23+2+47+32+40 = 144. */
    snprintf(msg,sizeof msg,"%.23s; %.47s%.32s%.40s", clk,nws,wxs,geo);
    /* A cancelled run is neither a success nor a failure, and must not be dressed
     * as either: "Done" would claim work that was never attempted, and "failed"
     * would send someone debugging a network that was fine. Say what happened,
     * and say what DID land -- the collections that finished before the stop are
     * genuinely synced and the user should know not to worry about them. */
    if(hs_stop()){
        snprintf(msg,sizeof msg,"Cancelled - %.23s%s", clk,
                 s_news_added > 0 ? "; news fetched" : "");
        s_was_cancelled = 1;
    }
    ESP_LOGI(TAG,"%s",msg);
    setst(msg);
    hs_heap("sync-done");  /* before Wi-Fi goes away */
    wifi_down();           /* also closes the last keep-alive connection */
    /* Back in Mode A. If largestDMA here is much smaller than it was at "wifi-up",
     * a Wi-Fi cycle fragments the DMA region -- which is what would break a plan to
     * bring Wi-Fi up before LVGL allocates its contiguous draw buffer. */
    hs_heap("wifi-down");
    /* HOW BIG SHOULD THIS TASK'S STACK BE? Its stack is heap, and on this device
     * heap is exactly what the mbedTLS handshake ran out of -- so an over-sized
     * stack is not free caution, it is the thing that breaks the sync. This is
     * the measurement that lets HOTSYNC_STACK be a number rather than a guess:
     * the high-water mark is the smallest the stack ever got, in bytes. */
    ESP_LOGI(TAG,"stack: %u bytes of %u still free at the end (phase low-water %u)",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)HOTSYNC_STACK, s_stack_lo);
    /* Clear the request BEFORE dropping s_busy: hotsync_cancel_pending() is
     * (s_busy && s_cancel), and clearing in the other order would leave a
     * one-instruction window where a new run could be born already cancelled. */
    s_cancel = 0;
    s_busy = 0;
    vTaskDelete(NULL);
}

/* ---- Wi-Fi scan ---------------------------------------------------------- */
static WifiAP s_scan[WIFI_SCAN_MAX];
static int    s_scan_n, s_scan_done;

int           wifi_scan_busy(void){ return s_busy && !s_scan_done; }
int           wifi_scan_done(void){ return s_scan_done; }
int           wifi_scan_count(void){ return s_scan_n; }
const WifiAP *wifi_scan_get(int i){
    return (i >= 0 && i < s_scan_n) ? &s_scan[i] : NULL;
}

/* Keep the strongest sighting of each name. One network on 2.4 and 5 GHz is two
 * records here and one network to the person choosing it, and showing it twice
 * would make the list look broken in the one place it has to look trustworthy. */
static void scan_add(const wifi_ap_record_t *r){
    const char *ssid = (const char *)r->ssid;
    if(!ssid[0]) return;                       /* hidden network: nothing to tap */
    for(int i = 0; i < s_scan_n; i++){
        if(strcmp(s_scan[i].ssid, ssid)) continue;
        if(r->rssi > s_scan[i].rssi) s_scan[i].rssi = r->rssi;
        return;
    }
    if(s_scan_n >= WIFI_SCAN_MAX) return;
    snprintf(s_scan[s_scan_n].ssid, sizeof s_scan[s_scan_n].ssid, "%s", ssid);
    s_scan[s_scan_n].rssi   = r->rssi;
    s_scan[s_scan_n].secure = r->authmode != WIFI_AUTH_OPEN;
    s_scan_n++;
}

static void scan_task(void *arg){
    (void)arg;
    /* The record array is STATIC, not on this task's stack: wifi_ap_record_t is
     * the best part of 80 bytes and a dozen of them is a kilobyte, which is a
     * real fraction of HOTSYNC_STACK. */
    static wifi_ap_record_t recs[WIFI_SCAN_MAX];
    s_scan_n = 0;
    if(!radio_up()){ setst("Wi-Fi hardware did not start"); goto done; }

    wifi_scan_config_t sc = { 0 };             /* every channel, active scan */
    if(esp_wifi_scan_start(&sc, true) != ESP_OK){ setst("Scan failed"); goto done; }

    uint16_t n = WIFI_SCAN_MAX;
    if(esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK){ setst("Scan failed"); goto done; }
    for(int i = 0; i < (int)n; i++) scan_add(&recs[i]);

    /* strongest first: an insertion sort over at most twelve entries */
    for(int i = 1; i < s_scan_n; i++){
        WifiAP t = s_scan[i]; int k = i - 1;
        while(k >= 0 && s_scan[k].rssi < t.rssi){ s_scan[k+1] = s_scan[k]; k--; }
        s_scan[k+1] = t;
    }
    if(s_scan_n) setst("Tap a network");
    else         setst("No networks found");

done:
    esp_wifi_scan_stop();
    wifi_down();
    s_scan_done = 1;
    s_busy = 0;
    vTaskDelete(NULL);
}

void wifi_scan_start(void){
    if(s_busy) return;
    s_busy = 1; s_scan_done = 0; s_scan_n = 0;
    setst("Looking for networks...");
    if(xTaskCreate(scan_task, "wifiscan", HOTSYNC_STACK, NULL, 4, NULL) != pdPASS){
        setst("Could not start the scan"); s_scan_done = 1; s_busy = 0;
    }
}

void hotsync_start(void){
    if(s_busy) return;
    s_busy = 1;
    s_cancel = 0;              /* a fresh run is never born cancelled */
    s_was_cancelled = 0;
    s_wifi_problem = HS_WIFI_OK;
    setst("Starting...");
    /* HOTSYNC_STACK: see its note. The stack is malloc'd from the DRAM heap, so
     * an overflow corrupts adjacent heap metadata rather than tripping a canary. */
    if(xTaskCreate(hotsync_task, "hotsync", HOTSYNC_STACK, NULL, 4, NULL) != pdPASS){
        setst("Could not start sync task"); s_busy = 0;
    }
}
