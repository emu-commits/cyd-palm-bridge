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

/* ---- per-app sync targets ------------------------------------------------
 * HotSync walks this table, syncing each configured app to its own iCloud
 * collection. Back-compat: an older secrets.h that only defines SYNC_COLL/
 * SYNC_KIND still syncs the Date Book exactly as before; To Do / Address stay
 * off until their collections are filled in. Memo has no iCloud DAV surface. */
#ifndef SYNC_KIND
#define SYNC_KIND KIND_CAL
#endif
#ifndef SYNC_CAL_COLL
#define SYNC_CAL_COLL SYNC_COLL
#endif
#ifndef SYNC_TODO_COLL
#define SYNC_TODO_COLL ""
#endif
#ifndef SYNC_TODO_PDB
#define SYNC_TODO_PDB "/sdcard/ToDoDB.pdb"
#endif
#ifndef SYNC_CARD_COLL
#define SYNC_CARD_COLL ""
#endif
#ifndef SYNC_CARD_PDB
#define SYNC_CARD_PDB "/sdcard/AddressDB.pdb"
#endif
#ifndef DAV_CARD_BASE
#define DAV_CARD_BASE "https://contacts.icloud.com"
#endif

typedef struct {
    const char *name;     /* label for the status line            */
    const char *pdb;      /* PDB path on SD                        */
    const char *coll;     /* iCloud collection ("" => skip)        */
    int         kind;     /* KIND_CAL | KIND_TODO | KIND_CARD      */
    const char *map;      /* per-collection state map on SD        */
    int         card;     /* 1 => CardDAV (uses the contacts host) */
} SyncTgt;

static const SyncTgt s_tgts[] = {
    { "Date Book", SYNC_PDB,      SYNC_CAL_COLL,  SYNC_KIND,  "/sdcard/state/cal.map",  0 },
    { "To Do",     SYNC_TODO_PDB, SYNC_TODO_COLL, KIND_TODO,  "/sdcard/state/todo.map", 0 },
    { "Address",   SYNC_CARD_PDB, SYNC_CARD_COLL, KIND_CARD,  "/sdcard/state/card.map", 1 },
};
#define N_TGTS ((int)(sizeof s_tgts / sizeof s_tgts[0]))

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
    wifi_config_t wc = { 0 };
    strncpy((char*)wc.sta.ssid, WIFI_SSID, sizeof wc.sta.ssid);
    strncpy((char*)wc.sta.password, WIFI_PASS, sizeof wc.sta.password);
    if(esp_wifi_set_mode(WIFI_MODE_STA)!=ESP_OK) return 0;
    if(esp_wifi_set_config(WIFI_IF_STA, &wc)!=ESP_OK) return 0;
    if(esp_wifi_start()!=ESP_OK) return 0;
    EventBits_t b = xEventGroupWaitBits(s_evt, WIFI_OK|WIFI_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(25000));
    (void)s_netif_inited;
    return (b & WIFI_OK) ? 1 : 0;
}
static void wifi_down(void){
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
    return (ti.tm_year+1900) >= 2024;
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

    DavCtx d; memset(&d,0,sizeof d);
    snprintf(d.base,sizeof d.base,"%s",DAV_BASE);
    snprintf(d.user,sizeof d.user,"%s",DAV_USER);
    snprintf(d.pass,sizeof d.pass,"%s",DAV_PASS);
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
    /* count configured apps so the status line can show progress [k/M]; with
     * three collections the per-app step is the coarse progress indicator (a
     * finer intra-collection bar would need a callback through sync_collection). */
    int ntgt=0; for(int i=0;i<N_TGTS;i++) if(s_tgts[i].coll && s_tgts[i].coll[0]) ntgt++;
    int step=0;
    for(int i=0;i<N_TGTS;i++){
        const SyncTgt *t=&s_tgts[i];
        if(!t->coll || !t->coll[0]) continue;      /* app not configured -> skip */
        step++;

        DavCtx *ctx=&d;
        if(t->card){
            if(!card_ready){
                memset(&dcard,0,sizeof dcard);
                snprintf(dcard.base,sizeof dcard.base,"%s",DAV_CARD_BASE);
                snprintf(dcard.user,sizeof dcard.user,"%s",DAV_USER);
                snprintf(dcard.pass,sizeof dcard.pass,"%s",DAV_PASS);
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
        ESP_LOGI(TAG,"sync %s coll=%s pdb=%s heap=%lu largest=%lu",t->name,t->coll,t->pdb,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        SyncStats st={0};
        int n = sync_collection(ctx, t->pdb, t->pdb, t->coll, t->kind, t->map, POL_SERVER, &st);
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

    if(did==0 && failed>0)
        snprintf(msg,sizeof msg,"Sync failed - low memory (heap %lu)",
                 (unsigned long)esp_get_free_heap_size());
    else
        snprintf(msg,sizeof msg,"Done: +%d~%d-%d up +%d~%d-%d down%s",
                 tot.pushNew,tot.pushMod,tot.pushDel, tot.pullNew,tot.pullMod,tot.pullDel,
                 (failed||protec)?" (some skipped)":"");
    ESP_LOGI(TAG,"%s",msg);
    setst(msg);
    wifi_down();
    s_busy = 0;
    vTaskDelete(NULL);
}

void hotsync_start(void){
    if(s_busy) return;
    s_busy = 1;
    setst("Starting...");
    /* 16 KB stack: the sync engine's big emit buffers now live in .bss (g_body)
     * and the object-fetch buffer is static, so the deepest path is a TLS
     * handshake (~8 KB) plus small frames. A 32 KB stack fixed the earlier
     * overflow but starved the heap of the contiguous block sync_collection needs
     * for its S/Out structs; 16 KB restores that headroom without overflowing. */
    if(xTaskCreate(hotsync_task, "hotsync", 20480, NULL, 4, NULL) != pdPASS){
        setst("Could not start sync task"); s_busy = 0;
    }
}
