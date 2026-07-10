/* lvgl_port.c -- LVGL bound to the CYD ILI9341 (display.c) + XPT2046 (touch.c).
 * Partial-buffer render mode (no full framebuffer -- the no-PSRAM rule). */
#include "lvgl_port.h"
#include "display.h"
#include "touch.h"
#include "power.h"
#include "appcfg.h"
#include "hotsync.h"
#include "lv_font_palm.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lvgl";

/* set for one touch-stroke right after a wake, so the tap that lights the screen
 * back up is swallowed (not delivered to a button) -- PalmOS wake-tap behavior. */
static volatile int g_swallow_tap = 0;

/* partial draw buffer: 40 rows. ~19KB, DMA-capable. */
#define BUF_ROWS 40

static uint32_t tick_cb(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map){
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)w * h);   /* LE render -> BE for ILI9341 */
    display_blit(area->x1, area->y1, w, h, px_map);
    lv_display_flush_ready(disp);
}

static void indev_cb(lv_indev_t *indev, lv_indev_data_t *data){
    (void)indev;
    static int32_t lx = 0, ly = 0;
    int x, y;
    if(tp_read(&x, &y)){
        lx = x; ly = y;
        /* swallow the wake tap: while the finger that woke the screen is still
         * down, report RELEASED so it never activates a widget. */
        if(g_swallow_tap){ data->point.x = lx; data->point.y = ly;
                           data->state = LV_INDEV_STATE_RELEASED; return; }
        data->point.x = x; data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        g_swallow_tap = 0;               /* finger lifted -> next tap is real */
        data->point.x = lx; data->point.y = ly;
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* idle backlight management, called once per run-loop iteration. When the screen
 * is on, blank it after cfg->backlight_sec of LVGL inactivity; when it's off,
 * poll the raw touch panel (LVGL inactivity won't advance with the display idle)
 * and wake on the first press, swallowing that tap. Returns 1 while blanked so
 * the caller can idle more slowly (deeper light-sleep, slower wake polling). */
static int idle_step(void){
    int secs = appcfg()->backlight_sec;
    /* never blank during a sync: the user is watching the progress line, and the
     * priority-4 sync task can starve this wake-poll -- a screen that blanked
     * mid-sync wouldn't relight on a tap until the sync finished. Keep it lit and
     * hold the idle timer at zero so it can't trip while syncing. */
    if(hotsync_busy()){
        if(power_screen_off()) power_backlight(1);
        lv_display_trigger_activity(NULL);
        return 0;
    }
    if(power_screen_off()){
        if(tp_pressed()){ power_backlight(1); g_swallow_tap = 1; }
        return power_screen_off();
    }
    if(secs > 0){
        uint32_t idle = lv_display_get_inactive_time(NULL);   /* ms since activity */
        if(idle > (uint32_t)secs * 1000){ power_backlight(0); return 1; }
    }
    return 0;
}

void lvgl_port_init(void){
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    size_t buf_bytes = (size_t)LCD_W * BUF_ROWS * 2;
    void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    if(!buf){ ESP_LOGE(TAG, "draw buffer alloc failed (%u bytes)", (unsigned)buf_bytes); return; }
    lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);

    /* monochrome theme (black on white, flat, thin borders) = the PalmOS look */
    lv_display_set_theme(disp, lv_theme_mono_init(disp, false, &lv_font_palm));

    ESP_LOGI(TAG, "LVGL up: %dx%d, %u-byte partial buffer", LCD_W, LCD_H, (unsigned)buf_bytes);
}

void lvgl_port_run(void){
    while(1){
        uint32_t next = lv_timer_handler();      /* ms until next work */
        int off = idle_step();                   /* backlight off/on + wake */
        if(off){
            /* screen blanked: nothing to draw. Idle in ~120 ms slices so the SoC
             * light-sleeps deeply between wake-polls (120 ms touch latency is
             * imperceptible for waking a PDA). */
            vTaskDelay(pdMS_TO_TICKS(120));
        } else {
            if(next > 30) next = 30;
            vTaskDelay(pdMS_TO_TICKS(next ? next : 5));
        }
    }
}
