/* lvgl_port.c -- LVGL bound to the CYD ILI9341 (display.c) + XPT2046 (touch.c).
 * Partial-buffer render mode (no full framebuffer -- the no-PSRAM rule). */
#include "lvgl_port.h"
#include "display.h"
#include "touch.h"
#include "power.h"
#include "appcfg.h"
#include "hotsync.h"
#include "ui.h"           /* ui_show_lock() / ui_owns_backlight() */
#include "tapsplit.h"     /* recover the lift between two fast taps */
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

/* Set by the wake-poll, cleared by the run loop one render pass later. The wake
 * tap refreshes the dashboard while the panel is still dark and the backlight
 * comes up only after that repaint has been flushed, so the first thing the eye
 * lands on is the current dashboard -- never the previous screen, and never a
 * dashboard showing the time it was when the device went to sleep. */
static int g_wake_pending = 0;

/* partial draw buffer: 40 rows. ~19KB, DMA-capable. */
#define BUF_ROWS 40
/* ...and 6 rows (~2.8 KB) for the duration of a HotSync. THE MEASUREMENT: with
 * Wi-Fi up there were 28 KB of heap free and the mbedTLS handshake bottomed out
 * at 88 bytes, so nine of ten feed fetches and the iCloud login all failed with
 * ESP_ERR_HTTP_CONNECT. This buffer is the single largest contiguous block the
 * UI holds, and a sync is the one time nothing needs to be drawn fast: the
 * HotSync screen is a logo and a status line. Handing ~16 KB of DMA-capable heap
 * to the handshake for the length of the sync costs a slower status repaint and
 * buys the connection. Swapped on the LVGL task between lv_timer_handler()
 * calls, and flush_cb is synchronous (blit then flush_ready), so no flush can be
 * in flight across the swap. */
#define BUF_ROWS_SYNC 6

static uint32_t tick_cb(void){ return (uint32_t)(esp_timer_get_time() / 1000); }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map){
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)w * h);   /* LE render -> BE for ILI9341 */
    display_blit(area->x1, area->y1, w, h, px_map);
    lv_display_flush_ready(disp);
}

/* How often the panel is read, and how far a press may jump between two reads
 * before it counts as a lift and a new tap (tapsplit.h). LVGL's default is the
 * refresh period, 33 ms -- long enough that the lift between two quick taps on
 * the Calculator fell between samples and the keys were lost. A read costs
 * ~0.25 ms when nothing is touching the panel (one median-of-5 Z1). */
#define TOUCH_READ_MS  10
#define TOUCH_JUMP_PX  24          /* keys are >= 44 px apart; a finger moves ~3 */

static void indev_cb(lv_indev_t *indev, lv_indev_data_t *data){
    (void)indev;
    static int32_t lx = 0, ly = 0;
    static TapSplit ts;
    int x, y;
    if(tp_read(&x, &y)){
        lx = x; ly = y;
        /* swallow the wake tap: while the finger that woke the screen is still
         * down, report RELEASED so it never activates a widget. */
        if(g_swallow_tap){ data->point.x = lx; data->point.y = ly;
                           data->state = LV_INDEV_STATE_RELEASED; return; }
        int ox, oy;
        int down = tapsplit_step(&ts, 1, x, y, ui_discrete_taps(), TOUCH_JUMP_PX, &ox, &oy);
        data->point.x = ox; data->point.y = oy;
        data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    } else {
        int ox, oy;
        tapsplit_step(&ts, 0, lx, ly, 0, TOUCH_JUMP_PX, &ox, &oy);
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
    /* Coach's end-of-session flash owns the backlight: don't blank under it, and
     * don't read its dark phases as a screen someone needs woken. Note what this
     * does NOT do -- unlike the sync guard below it leaves the idle timer alone,
     * because the flash reads that timer to tell whether it has been noticed yet.
     * Resetting it here would make every flash stop after one phase. The flash
     * restarts the countdown itself when it ends. */
    if(ui_owns_backlight()) return 0;
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
        if(tp_pressed()){
            g_swallow_tap = 1;
            lv_display_trigger_activity(NULL);   /* the swallowed tap still counts as use */
            ui_show_lock();                      /* already up: repaints it, in the dark */
            g_wake_pending = 1;                  /* light it once that repaint has landed */
        }
        return power_screen_off();
    }
    if(secs > 0){
        uint32_t idle = lv_display_get_inactive_time(NULL);   /* ms since activity */
        if(idle > (uint32_t)secs * 1000){
            /* Blank FIRST, then raise the lock: building it behind a dark panel is
             * what makes the wake instant. Doing it the other way round -- raising
             * the lock on wake, as this did -- shows the previous screen for the
             * frame or two the dashboard takes to build and paint. */
            power_backlight(0);
            ui_show_lock();
            return 1;
        }
    }
    return 0;
}

static lv_display_t *g_disp;
static void *g_buf;              /* the live draw buffer  */
static int   g_buf_rows;         /* how many rows it holds */

/* Swap the draw buffer to `rows` deep. Any failure LEAVES THE CURRENT BUFFER IN
 * PLACE and returns 0 -- a sync that cannot free the memory is worth much less
 * than a UI that cannot draw, so this never gives up the buffer it has until it
 * holds the replacement. */
static int resize_draw_buf(int rows){
    if(!g_disp || rows == g_buf_rows) return 0;
    size_t bytes = (size_t)LCD_W * rows * 2;
    void *nb = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    if(!nb){ ESP_LOGW(TAG,"draw buffer resize to %d rows failed; keeping %d", rows, g_buf_rows); return 0; }
    void *old = g_buf;
    lv_display_set_buffers(g_disp, nb, NULL, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    g_buf = nb; g_buf_rows = rows;
    heap_caps_free(old);
    lv_obj_invalidate(lv_screen_active());     /* repaint through the new buffer */
    ESP_LOGI(TAG,"draw buffer now %d rows (%u bytes), heap free=%lu largestDMA=%lu",
             rows, (unsigned)bytes, (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return 1;
}

void lvgl_port_init(void){
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    size_t buf_bytes = (size_t)LCD_W * BUF_ROWS * 2;
    void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
    if(!buf){ ESP_LOGE(TAG, "draw buffer alloc failed (%u bytes)", (unsigned)buf_bytes); return; }
    g_disp = disp; g_buf = buf; g_buf_rows = BUF_ROWS;
    lv_display_set_buffers(disp, buf, NULL, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);
    lv_timer_set_period(lv_indev_get_read_timer(indev), TOUCH_READ_MS);

    /* monochrome theme (black on white, flat, thin borders) = the PalmOS look */
    lv_display_set_theme(disp, lv_theme_mono_init(disp, false, &lv_font_palm));

    ESP_LOGI(TAG, "LVGL up: %dx%d, %u-byte partial buffer", LCD_W, LCD_H, (unsigned)buf_bytes);

    /* Report the object pool's REAL size, measured rather than assumed.
     *
     * Worth a line of boot log because the number is load-bearing and is set two
     * indirections away from where it matters: sdkconfig's
     * CONFIG_LV_MEM_SIZE_KILOBYTES is mapped to CONFIG_LV_MEM_SIZE by LVGL's
     * lv_conf_kconfig.h, and only then to LV_MEM_SIZE. LVGL 9.6 broke exactly
     * that mapping (it deprecates the KILOBYTES form and silently prefers a
     * 64 KB default), which is why firmware/main/idf_component.yml pins the
     * version. If that pin is ever raised, this line is the check: it must still
     * say 32768, and a pool that has quietly become 64 KB will not fit DRAM. */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "lvgl pool: %u bytes total, %u free (largest block %u)",
             (unsigned)mon.total_size, (unsigned)mon.free_size,
             (unsigned)mon.free_biggest_size);
}

/* ---- the pool, watched on the device (robustness 6) ---------------------------
 * The LVGL object pool is fixed at 31 KB and running out of it does not fail
 * politely: LV_ASSERT_MALLOC spins and the watchdog resets the device, and the
 * UART says nothing about where. The simulator measures the pool at every
 * screenshot and names the screen at the low-water mark; this is the same
 * measurement on real hardware, where a field report is the only report there
 * is. A line when the low-water mark drops by a meaningful step, naming the
 * screen, and a warning under the same 3 KB floor the smoke gate enforces.
 * Once a second: lv_mem_monitor walks the pool, which is not free. */
#define POOL_FLOOR 3072
#define POOL_STEP  512
static void pool_watch(void){
    static int64_t last;
    static uint32_t low = UINT32_MAX;
    int64_t now = esp_timer_get_time();
    if(now - last < 1000000) return;
    last = now;
    lv_mem_monitor_t m;
    lv_mem_monitor(&m);
    uint32_t f = (uint32_t)m.free_size;
    if(low != UINT32_MAX && f + POOL_STEP > low) return;   /* no new low worth a line */
    low = f;
    if(f < POOL_FLOOR)
        ESP_LOGW(TAG, "pool LOW: %u bytes free (largest %u) on \"%s\"",
                 (unsigned)f, (unsigned)m.free_biggest_size, ui_screen_name());
    else
        ESP_LOGI(TAG, "pool low-water: %u bytes free (largest %u) on \"%s\"",
                 (unsigned)f, (unsigned)m.free_biggest_size, ui_screen_name());
}

void lvgl_port_run(void){
    while(1){
        uint32_t next = lv_timer_handler();      /* ms until next work */
        pool_watch();
        /* the wake tap's repaint has now been flushed to the panel -- light it */
        if(g_wake_pending){ g_wake_pending = 0; power_backlight(1); }

        /* lend the draw buffer's memory to the sync for as long as it runs */
        static int was_syncing = 0;
        int syncing = hotsync_busy();
        if(syncing != was_syncing){
            resize_draw_buf(syncing ? BUF_ROWS_SYNC : BUF_ROWS);
            was_syncing = syncing;
        }
        int off = idle_step();                   /* backlight off/on + wake */
        if(off && !g_wake_pending){
            /* screen blanked: nothing to draw. Idle in ~120 ms slices so the SoC
             * light-sleeps deeply between wake-polls (120 ms touch latency is
             * imperceptible for waking a PDA). A pending wake skips this and takes
             * the fast path below, so the one extra pass costs a frame, not 120 ms. */
            vTaskDelay(pdMS_TO_TICKS(120));
        } else {
            if(next > 30) next = 30;
            vTaskDelay(pdMS_TO_TICKS(next ? next : 5));
        }
    }
}
