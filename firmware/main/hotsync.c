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
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "hotsync";
static volatile int s_busy;
static char s_status[80] = "Ready";
static void setst(const char *s){ snprintf(s_status, sizeof s_status, "%s", s); }

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
static void wifi_down(void){
    dav_disconnect();   /* close any keep-alive connection before the TLS/socket stack goes away */
    esp_wifi_stop();
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_h_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_h_ip);
    esp_wifi_deinit();
    if(s_evt){ vEventGroupDelete(s_evt); s_evt=NULL; }
}

static int clock_ok(void){
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if(esp_netif_sntp_init(&cfg)!=ESP_OK) return 0;
    for(int i=0;i<15;i++) if(esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000))==ESP_OK) break;
    esp_netif_sntp_deinit();
    time_t now=0; time(&now); struct tm ti; gmtime_r(&now,&ti);
    if((ti.tm_year+1900) < 2024) return 0;
    clock_checkpoint();   /* SNTP just set real time -> persist it durably (survives power-off) */
    return 1;
}

static char *abspath(char *href, DavCtx *d){
    if(strncmp(href,"http",4)==0){ char *p=strstr(href,"://");
        if(p){ char *s=strchr(p+3,'/'); if(s){ int n=(int)(s-href);
            if(n<(int)sizeof d->base){ memcpy(d->base,href,n); d->base[n]=0; } return s; } } }
    return href;
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
    ESP_LOGI(TAG,"wifi up: free heap=%lu largest block=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    setst("Setting clock...");
    if(!clock_ok()){ setst("Clock/SNTP failed"); wifi_down(); s_busy=0; vTaskDelete(NULL); return; }

    const Config *cfg = appcfg();
    ESP_LOGI(TAG,"config source: %s", appcfg_from_sd() ? "/sdcard/config.ini" : "secrets.h (no config.ini)");

    DavCtx d; memset(&d,0,sizeof d);
    snprintf(d.base,sizeof d.base,"%s",cfg->dav_base);
    snprintf(d.user,sizeof d.user,"%s",cfg->dav_user);
    snprintf(d.pass,sizeof d.pass,"%s",cfg->dav_pass);
    (void)abspath;
    char msg[80];

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
        snprintf(msg,sizeof msg,"Login failed (HTTP %d) - check user/pass",dav_last_status);
        ESP_LOGE(TAG,"%s",msg); setst(msg); wifi_down(); s_busy=0; vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG,"principal: %s",principal);

    /* Address book (CardDAV) lives on a separate iCloud host; resolve it lazily
     * the first time a card target is reached. Same credentials as caldav. */
    DavCtx dcard; int card_ready=0;

    SyncStats tot={0};
    int did=0, failed=0, protec=0;
    ConflictPolicy pol = (ConflictPolicy)cfg->policy;
    /* count configured apps so the status line can show progress [k/M]; with
     * three collections the per-app step is the coarse progress indicator (a
     * finer intra-collection bar would need a callback through sync_collection). */
    int ntgt=0; for(int i=0;i<N_APPS;i++){ const char*c=app_coll(cfg,i); if(c&&c[0]) ntgt++; }
    int step=0;
    setprog(0);
    sync_set_progress(hs_prog_cb, NULL);           /* engine drives the intra-collection bar */
    for(int i=0;i<N_APPS;i++){
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
                } else ESP_LOGW(TAG,"contacts host resolve failed (HTTP %d)",dav_last_status);
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
    setprog(100);
    if(did==0 && failed>0)
        snprintf(msg,sizeof msg,"Sync failed - low memory (heap %lu)",
                 (unsigned long)esp_get_free_heap_size());
    else
        snprintf(msg,sizeof msg,"Done: +%d~%d-%d up +%d~%d-%d down%s",
                 tot.pushNew,tot.pushMod,tot.pushDel, tot.pullNew,tot.pullMod,tot.pullDel,
                 (failed||protec)?" (some skipped)":"");
    ESP_LOGI(TAG,"%s",msg);
    setst(msg);
    wifi_down();           /* also closes the last collection's keep-alive connection */
    s_busy = 0;
    vTaskDelete(NULL);
}

/* ---- collection discovery (Preferences) ----------------------------------
 * Same background-task slot as the sync (guarded by s_busy). Results land in a
 * small fixed array; the UI reads them after hotsync_discover_done(). */
#define MAX_DISC 12
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
    if(s_disc_n >= MAX_DISC) return;
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
    if(xTaskCreate(discover_task, "discover", 32768, NULL, 4, NULL) != pdPASS){
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
    if(xTaskCreate(hotsync_task, "hotsync", 32768, NULL, 4, NULL) != pdPASS){
        setst("Could not start sync task"); s_busy = 0;
    }
}
