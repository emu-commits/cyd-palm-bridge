/* lvgl_port.c -- LVGL bound to the CYD ILI9341 (display.c) + XPT2046 (touch.c).
 * Partial-buffer render mode (no full framebuffer -- the no-PSRAM rule). */
#include "lvgl_port.h"
#include "display.h"
#include "touch.h"
#include "lv_font_palm.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lvgl";

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
        data->point.x = x; data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->point.x = lx; data->point.y = ly;
        data->state = LV_INDEV_STATE_RELEASED;
    }
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
        if(next > 30) next = 30;
        vTaskDelay(pdMS_TO_TICKS(next ? next : 5));
    }
}
