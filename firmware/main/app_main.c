/* app_main.c -- headless CYD Palm -> iCloud sync (first on-device bring-up).
 *
 * Flow: Wi-Fi STA -> SNTP (no RTC, TLS needs the clock) -> mount SD ->
 * resolve the iCloud effective host -> sync_collection() one collection between
 * a .pdb on the SD card and iCloud, over the shared codec + sync engine.
 *
 * Discovery/category-routing and a UI are deliberately out of scope for this
 * first flash: the point is to prove the codec + reconcile + TLS DAV + SD stack
 * end-to-end on hardware. Collection path comes from secrets.h for now.
 */
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include <sys/stat.h>

#include "dav.h"
#include "sync.h"
#include "display.h"
#include "touch.h"
#include "lvgl_port.h"
#include "ui.h"
#include "data.h"
#include "secrets.h"
#include "clock.h"
#include "appcfg.h"

static const char *TAG = "app";

/* ----- CYD (ESP32-2432S028R) SD-card SPI pins. Adjust if your board differs;
 * the SD slot is separate from the ILI9341 display SPI. ----- */
#define SD_PIN_MOSI 23
#define SD_PIN_MISO 19
#define SD_PIN_SCLK 18
#define SD_PIN_CS    5
#define SD_SPI_HOST SPI2_HOST

/* ---------------- Wi-Fi ---------------- */
static EventGroupHandle_t s_wifi_evt;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retries;

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data){
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START){
        esp_wifi_connect();
    } else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED){
        if(s_retries++ < 10){ ESP_LOGW(TAG,"wifi retry %d",s_retries); esp_wifi_connect(); }
        else xEventGroupSetBits(s_wifi_evt, WIFI_FAIL_BIT);
    } else if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP){
        ip_event_got_ip_t *e = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG,"got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries=0; xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect(void){
    s_wifi_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL, NULL));
    wifi_config_t wc = { 0 };
    strncpy((char*)wc.sta.ssid, WIFI_SSID, sizeof wc.sta.ssid);
    strncpy((char*)wc.sta.password, WIFI_PASS, sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    EventBits_t b = xEventGroupWaitBits(s_wifi_evt, WIFI_CONNECTED_BIT|WIFI_FAIL_BIT,
                                        pdFALSE, pdFALSE, portMAX_DELAY);
    return (b & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

/* ---------------- SNTP (clock for TLS cert validity) ---------------- */
static esp_err_t clock_sync(void){
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
    for(int i=0;i<20;i++){
        if(esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000))==ESP_OK) break;
        ESP_LOGI(TAG,"waiting for SNTP... (%d)",i+1);
    }
    time_t now=0; time(&now);
    struct tm ti; gmtime_r(&now,&ti);
    ESP_LOGI(TAG,"UTC time: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year+1900,ti.tm_mon+1,ti.tm_mday,ti.tm_hour,ti.tm_min,ti.tm_sec);
    return (ti.tm_year+1900 >= 2024) ? ESP_OK : ESP_FAIL;
}

/* ---------------- SD card ---------------- */
static sdmmc_card_t *s_card;
static esp_err_t sd_mount(void){
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false, .max_files = 6, .allocation_unit_size = 16*1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    spi_bus_config_t bus = {
        .mosi_io_num=SD_PIN_MOSI, .miso_io_num=SD_PIN_MISO, .sclk_io_num=SD_PIN_SCLK,
        .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=4096,
    };
    /* SPI_DMA_CH_AUTO (not the fixed SDSPI_DEFAULT_DMA) so SD grabs the DMA channel
     * the display's auto-picked one didn't take (ESP32 has two). */
    esp_err_t e = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if(e!=ESP_OK){ ESP_LOGE(TAG,"spi bus init: %s",esp_err_to_name(e)); return e; }
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = SD_PIN_CS; dev.host_id = SD_SPI_HOST;
    e = esp_vfs_fat_sdspi_mount("/sdcard", &host, &dev, &mcfg, &s_card);
    if(e!=ESP_OK){ ESP_LOGE(TAG,"sd mount: %s",esp_err_to_name(e)); return e; }
    ESP_LOGI(TAG,"SD mounted (%lluMB)", ((uint64_t)s_card->csd.capacity)*s_card->csd.sector_size/(1024*1024));
    mkdir("/sdcard/state", 0777);
    return ESP_OK;
}

static void dav_from_secrets(DavCtx*d){
    memset(d,0,sizeof *d);
    snprintf(d->base,sizeof d->base,"%s",DAV_BASE);
    snprintf(d->user,sizeof d->user,"%s",DAV_USER);
    snprintf(d->pass,sizeof d->pass,"%s",DAV_PASS);
}

/* resolve iCloud's per-user host once (caldav.icloud.com -> pNN-caldav...) */
static void resolve_host(DavCtx*d){
    char host[256]="";
    if(dav_effective_host(d,"/",host,sizeof host)==0 && host[0]){
        ESP_LOGI(TAG,"effective host: %s",host);
        snprintf(d->base,sizeof d->base,"%s",host);
    } else {
        ESP_LOGW(TAG,"could not resolve effective host (status %d); using %s",dav_last_status,d->base);
    }
}

/* If href is an absolute URL, repoint d->base at its origin and return the path;
 * else return href unchanged. iCloud returns absolute hrefs for home-sets on the
 * per-user host (mirrors absPath() in the host CLI). */
static char* abspath(char*href, DavCtx*d){
    if(strncmp(href,"http",4)==0){
        char*p=strstr(href,"://");
        if(p){ char*slash=strchr(p+3,'/');
            if(slash){ int n=(int)(slash-href);
                if(n<(int)sizeof d->base){ memcpy(d->base,href,n); d->base[n]=0; }
                return slash; } }
    }
    return href;
}

/* ---- discovery smoke test (no SD needed): proves WiFi+SNTP+TLS+PROPFIND ---- */
static void discover_cb(const char*href,int kind,const char*dn,void*ctx){
    (void)ctx;
    if(kind!='c' && kind!='a') return;   /* skip home-self, inbox/outbox, notifications */
    ESP_LOGI(TAG,"  [%c] %-24s %s", kind, dn&&dn[0]?dn:"(unnamed)", href);
}
static void run_discovery(void){
    DavCtx d; dav_from_secrets(&d);
    ESP_LOGI(TAG,"heap before discovery: %lu",(unsigned long)esp_get_free_heap_size());
    resolve_host(&d);
    char principal[512]="";
    if(dav_prop_href(&d,"/","<d:current-user-principal/>","",principal,sizeof principal)!=0 || !principal[0]){
        ESP_LOGE(TAG,"principal lookup failed (status %d) -- check DAV_USER/DAV_PASS",dav_last_status);
        return;
    }
    ESP_LOGI(TAG,"principal: %s",principal);
    char *ppath=abspath(principal,&d);
    char home[512]="";
    dav_prop_href(&d,ppath,"<c:calendar-home-set/>","xmlns:c=\"urn:ietf:params:xml:ns:caldav\"",home,sizeof home);
    if(!home[0]){ ESP_LOGE(TAG,"no calendar-home-set"); return; }
    char *hpath=abspath(home,&d);
    ESP_LOGI(TAG,"calendar-home: %s%s\ncalendars:",d.base,hpath);
    dav_list_collections(&d,hpath,discover_cb,NULL);
    ESP_LOGI(TAG,"discovery done. heap: %lu",(unsigned long)esp_get_free_heap_size());
}

/* ---------------- the sync ---------------- */
static void run_sync(void){
    DavCtx d; dav_from_secrets(&d);
    resolve_host(&d);

    ESP_LOGI(TAG,"heap before sync: %lu",(unsigned long)esp_get_free_heap_size());
    SyncStats st={0};
    int n = sync_collection(&d, SYNC_PDB, SYNC_PDB, SYNC_COLL, SYNC_KIND,
                            "/sdcard/state/cal.map", POL_SERVER, &st);
    ESP_LOGI(TAG,"sync done: %d records | +%d ~%d -%d push | +%d ~%d -%d pull | %d conflict | %d clean",
             n, st.pushNew,st.pushMod,st.pushDel, st.pullNew,st.pullMod,st.pullDel,
             st.conflicts, st.unchanged);
    ESP_LOGI(TAG,"heap after sync: %lu (min ever %lu)",
             (unsigned long)esp_get_free_heap_size(),(unsigned long)esp_get_minimum_free_heap_size());
}

void app_main(void){
    ESP_LOGI(TAG,"boot: free heap %lu, largest block %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    esp_err_t e = nvs_flash_init();
    if(e==ESP_ERR_NVS_NO_FREE_PAGES || e==ESP_ERR_NVS_NEW_VERSION_FOUND){
        ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* No RTC on this board: restore the wall clock from NVS so the date/time
     * survive a power cycle (a HotSync's SNTP later corrects it exactly). */
    clock_restore();

    /* U1: display bring-up -- draw a diagnostic test pattern first thing. */
    display_init();
    display_test_pattern();

    /* U2: touch. Load saved calibration; (re)calibrate only if none is stored or
     * the user is holding the screen at boot (force re-cal). */
    tp_init();
    if(tp_pressed() || !tp_cal_load()){
        ESP_LOGI(TAG,"U2 calibration: tap each white crosshair (TL, TR, BL)");
        tp_calibrate();
        tp_cal_save();
        ESP_LOGI(TAG,"calibration saved to NVS");
    } else {
        ESP_LOGI(TAG,"loaded touch calibration from NVS (hold screen at boot to re-cal)");
    }

    /* U4: mount the SD card and seed demo PDBs so the views have content. */
    if(sd_mount()==ESP_OK){
        data_seed_if_empty();
    } else {
        ESP_LOGW(TAG,"no SD card -- data views will be empty");
    }

    /* now that config.ini is readable, set the local timezone (so the Day view
     * shows the user's wall clock, not UTC) and start periodic clock checkpoints
     * so an abrupt power-off loses only ~2 min of accuracy. */
    clock_set_tz(appcfg()->timezone);
    clock_start_autosave();

    /* U3: bring up LVGL + the Palm app shell (never returns). */
    lvgl_port_init();
    ui_init();
    lvgl_port_run();

    if(wifi_connect()!=ESP_OK){ ESP_LOGE(TAG,"wifi failed; halting"); return; }
    if(clock_sync()!=ESP_OK){ ESP_LOGE(TAG,"clock not set; TLS would fail; halting"); return; }

    if(sd_mount()==ESP_OK){
        run_sync();
    } else {
        ESP_LOGW(TAG,"no SD card -- running discovery smoke test instead (transport check)");
        run_discovery();
    }

    ESP_LOGI(TAG,"idle. reset to run again.");
    while(1) vTaskDelay(pdMS_TO_TICKS(10000));
}
