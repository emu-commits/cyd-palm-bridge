/* power.c -- PWM backlight + automatic light-sleep (see power.h). */
#include "power.h"
#include "appcfg.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "power";

/* CYD backlight is on GPIO21 (same PIN_BL display.c drove high). */
#define BL_PIN       21
#define BL_MODE      LEDC_LOW_SPEED_MODE
#define BL_TIMER     LEDC_TIMER_0
#define BL_CHANNEL   LEDC_CHANNEL_0
#define BL_RES       LEDC_TIMER_8_BIT      /* 0..255 duty */

static int g_bright = 80;                  /* current "on" brightness 0..100 */
static int g_off     = 0;                  /* 1 while blanked by idle timeout */

static void apply_duty(int pct){
    if(pct < 0) pct = 0;
    if(pct > 100) pct = 100;
    uint32_t duty = (uint32_t)pct * 255 / 100;
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

void power_init(void){
    /* RTC8M-clocked so the PWM keeps running through light-sleep (the APB clock
     * stops in light-sleep; an APB-sourced LEDC would freeze the backlight). */
    ledc_timer_config_t t = {
        .speed_mode      = BL_MODE,
        .timer_num       = BL_TIMER,
        .duty_resolution = BL_RES,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_USE_RC_FAST_CLK,   /* RC_FAST (RTC8M): survives light-sleep */
    };
    if(ledc_timer_config(&t) != ESP_OK){
        /* fall back to auto clock if RTC8M can't hit the requested freq */
        t.clk_cfg = LEDC_AUTO_CLK; ledc_timer_config(&t);
    }
    ledc_channel_config_t ch = {
        .gpio_num   = BL_PIN,
        .speed_mode = BL_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    g_bright = appcfg()->brightness;
    apply_duty(g_bright);
    ESP_LOGI(TAG, "backlight PWM up: brightness=%d%%", g_bright);

#if CONFIG_PM_ENABLE
    /* Automatic light-sleep: the SoC drops to low power during the idle delays
     * between LVGL frames (and whenever the sync task is blocked). Wi-Fi + SD/SPI
     * drivers hold PM locks while active, so a sync is never interrupted. Needs
     * CONFIG_FREERTOS_USE_TICKLESS_IDLE (set in sdkconfig.defaults). */
    esp_pm_config_t pm = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40,                /* XTAL floor while idle */
        .light_sleep_enable = true,
    };
    if(esp_pm_configure(&pm) == ESP_OK)
        ESP_LOGI(TAG, "esp_pm: DFS %d->40 MHz + automatic light-sleep",
                 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    else
        ESP_LOGW(TAG, "esp_pm_configure failed -- light-sleep disabled");
#endif
}

void power_set_brightness(int pct){
    if(pct < 0) pct = 0;
    if(pct > 100) pct = 100;
    g_bright = pct;
    if(!g_off) apply_duty(g_bright);       /* if blanked, stay off until wake */
}

void power_backlight(int on){
    g_off = !on;
    apply_duty(on ? g_bright : 0);
}

int power_screen_off(void){ return g_off; }
