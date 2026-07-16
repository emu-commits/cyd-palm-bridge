/* sim_port.c -- see sim_port.h. */
#include "sim_port.h"
#include "lvgl.h"
#include "lv_font_palm.h"   /* authentic Palm font -> same mono theme call as the device */
#include <string.h>

/* ---- rendered output: full RGBA framebuffer the frontends read ---- */
static uint8_t s_fb[SIM_W * SIM_H * 4];
const uint8_t *sim_fb_ptr(void){ return s_fb; }

/* ---- injected time ---- */
static uint32_t s_tick_ms;
static uint32_t tick_cb(void){ return s_tick_ms; }

/* ---- injected pointer ---- */
static int32_t s_tx, s_ty, s_tdown;
void sim_touch(int x, int y, int down){ s_tx = x; s_ty = y; s_tdown = down; }
static void indev_cb(lv_indev_t *indev, lv_indev_data_t *data){
    (void)indev;
    data->point.x = s_tx;
    data->point.y = s_ty;
    data->state   = s_tdown ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* Device-parity draw buffer: 40 rows of RGB565, PARTIAL render mode -- the same
 * shape as firmware/main/lvgl_port.c (BUF_ROWS 40), so rendering exercises the
 * same partial-flush path and the 24 KB LVGL pool behaves like the device's. */
#define BUF_ROWS 40
static uint8_t s_draw_buf[SIM_W * BUF_ROWS * 2];

/* flush: RGB565 little-endian -> RGBA8888 into the frontend framebuffer.
 * (The device swaps to big-endian for the ILI9341 wire; the sim keeps LE.) */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map){
    const uint16_t *src = (const uint16_t *)px_map;
    for(int y = area->y1; y <= area->y2; y++){
        uint8_t *dst = s_fb + (y * SIM_W + area->x1) * 4;
        for(int x = area->x1; x <= area->x2; x++){
            uint16_t v = *src++;
            uint8_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            *dst++ = (uint8_t)(r << 3 | r >> 2);
            *dst++ = (uint8_t)(g << 2 | g >> 4);
            *dst++ = (uint8_t)(b << 3 | b >> 2);
            *dst++ = 0xFF;
        }
    }
    lv_display_flush_ready(disp);
}

void sim_init(void){
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(SIM_W, SIM_H);
    lv_display_set_buffers(disp, s_draw_buf, NULL, sizeof s_draw_buf,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);

    /* same theme call as the device port: mono, Palm system font */
    lv_display_set_theme(disp, lv_theme_mono_init(disp, false, &lv_font_palm));
}

void sim_step(int ms){
    /* advance in small slices so LVGL timers (indev polling, animations, the
     * UI's lv_timers) fire at their real cadence within the simulated span. */
    while(ms > 0){
        int slice = ms > 5 ? 5 : ms;
        s_tick_ms += (uint32_t)slice;
        ms -= slice;
        lv_timer_handler();
    }
}
