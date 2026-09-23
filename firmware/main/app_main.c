/* app_main.c -- headless CYD Palm -> iCloud sync (first on-device bring-up).
 *
 * Flow: Wi-Fi STA -> SNTP (no RTC, TLS needs the clock) -> mount SD ->
 * resolve the iCloud effective host -> sync_collection() one collection between
 * a .pdb on the SD card and iCloud, over the shared codec + sync engine.
 *
 * Discovery/category-routing and a UI are deliberately out of scope for this
 * first flash: the point is to prove the codec + reconcile + TLS DAV + SD stack
 * end-to-end on hardware.
 */
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include <sys/stat.h>

#include "display.h"
#include "touch.h"
#include "lvgl_port.h"
#include "power.h"
#include "ui.h"
#include "data.h"
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

/* ---- What used to live here, and why it is gone (2026-09-22) -------------
 * The first bring-up did its own Wi-Fi association, its own SNTP, its own
 * iCloud host resolution and its own one-collection sync, all from secrets.h,
 * in a tail below lvgl_port_run(). lvgl_port_run() does not return, so NONE OF
 * IT HAD EXECUTED SINCE THE UI LANDED -- roughly 150 lines that read like the
 * boot path and were not the boot path, including a second copy of the
 * effective-host and absolute-href logic that had drifted from the live one.
 *
 * hotsync.c does all of it now, on a background task, from config.ini rather
 * than compiled-in credentials. (There is no compile-time secrets.h at all any
 * more -- see appcfg.c.) git log -- firmware/main/app_main.c has the old version if the early
 * bring-up sequence is ever wanted for reference. */



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
        /* Two-way sync is live, so anything sitting in the local Date Book gets
         * pushed to the real account. The devtools "Add test events" seed must
         * not be part of that. Clearing it at boot -- before any HotSync can run
         * -- is what makes the ordering safe; with nothing to remove this does
         * not touch the file. */
        /* Two-way sync is live: a local database that reads short would be pushed
         * to the server AS DELETIONS. Say what is actually on the card first. */
        for(int ap=0; ap<3; ap++){
            static const char *NM[3] = { "Date Book", "Address", "To Do" };  /* APP_CAL, APP_ADDR, APP_TODO */
            int walked = 0; long bytes = 0;
            int hdr = data_db_stat(ap, &walked, &bytes);
            if(hdr < 0){ ESP_LOGW(TAG,"%s: no database on the card", NM[ap]); continue; }
            if(hdr != walked)
                ESP_LOGE(TAG,"%s DATABASE IS SHORT: header says %d records, only %d "
                             "readable (%ld bytes). Do not sync -- the missing %d would "
                             "be pushed as deletions.", NM[ap], hdr, walked, bytes, hdr-walked);
            else
                ESP_LOGI(TAG,"%s: %d records (%ld bytes)", NM[ap], hdr, bytes);
        }
        int nrm = data_remove_test_events();
        if(nrm > 0)
            ESP_LOGW(TAG,"removed %d seeded test event(s) of %d Date Book records",
                     nrm, data_testev_scanned());
        else if(nrm < 0)
            ESP_LOGE(TAG,"test-event cleanup REFUSED: %d Date Book records will not fit "
                         "the rewrite buffer, and a partial rewrite would lose real "
                         "appointments. Nothing was changed.", data_testev_scanned());
        else
            ESP_LOGI(TAG,"no seeded test events found (%d Date Book records examined)",
                     data_testev_scanned());
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
    /* U8 power: PWM backlight (configured brightness) + automatic light-sleep.
     * After LVGL/config are up so it can read appcfg() and own the backlight. */
    power_init();
    /* The drain experiment (Menu > Options > Power). Started after the SD mount
     * above, because a log that only exists over serial answers nothing: plugging
     * in USB is what ENDS a discharge, so the readings have to survive on the
     * card. */
    power_log_start();

    /* Never returns: this is the LVGL loop, and it is the last line of the boot
     * path for that reason. Anything written below it does not run. */
    lvgl_port_run();
}
