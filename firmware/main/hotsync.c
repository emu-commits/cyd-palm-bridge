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
#include "esp_log.h"

static const char *TAG = "hotsync";
static volatile int s_busy;
static char s_status[80] = "Ready";
static void setst(const char *s){ snprintf(s_status, sizeof s_status, "%s", s); }

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
    setst("Connecting Wi-Fi...");
    if(!wifi_up()){ setst("Wi-Fi failed"); wifi_down(); s_busy=0; vTaskDelete(NULL); return; }
    setst("Setting clock...");
    if(!clock_ok()){ setst("Clock/SNTP failed"); wifi_down(); s_busy=0; vTaskDelete(NULL); return; }

    DavCtx d; memset(&d,0,sizeof d);
    snprintf(d.base,sizeof d.base,"%s",DAV_BASE);
    snprintf(d.user,sizeof d.user,"%s",DAV_USER);
    snprintf(d.pass,sizeof d.pass,"%s",DAV_PASS);
    setst("Resolving host...");
    char host[256]="";
    if(dav_effective_host(&d,"/",host,sizeof host)==0 && host[0]) snprintf(d.base,sizeof d.base,"%s",host);
    (void)abspath;

    setst("Syncing Date Book...");
    SyncStats st={0};
    int n = sync_collection(&d, SYNC_PDB, SYNC_PDB, SYNC_COLL, SYNC_KIND,
                            "/sdcard/state/cal.map", POL_SERVER, &st);
    char msg[80];
    if(n < 0) snprintf(msg,sizeof msg,"Sync failed (check creds/coll)");
    else snprintf(msg,sizeof msg,"Done: +%d ~%d -%d up, +%d ~%d -%d down",
                  st.pushNew,st.pushMod,st.pushDel, st.pullNew,st.pullMod,st.pullDel);
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
    /* generous stack: TLS + sync run here */
    if(xTaskCreate(hotsync_task, "hotsync", 12288, NULL, 4, NULL) != pdPASS){
        setst("Could not start sync task"); s_busy = 0;
    }
}
