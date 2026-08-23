/* power.c -- PWM backlight + automatic light-sleep (see power.h). */
#include "power.h"
#include "appcfg.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
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

static void res_accrue(void);              /* drain-log residency, defined below */

static void apply_duty(int pct){
    if(pct < 0) pct = 0;
    if(pct > 100) pct = 100;
    uint32_t duty = (uint32_t)pct * 255 / 100;
    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

/* ======================= battery gauge (JP2 cell -> IO34) ===================
 * The board carries a TP4054 charge-management IC on the 2-pin JP2 seat and
 * brings BAT_ADC out to IO34 through a 2:1 divider, so a cell charges over USB
 * with no extra part and its voltage is readable on ADC1 channel 6.
 *
 * Three things make a naive read lie, and each is handled below:
 *   - The 12 dB attenuator is NOT linear. Raw counts scaled by 3300/4095 are
 *     off by >100 mV at the top of the range, which on a Li-ion curve is tens
 *     of percent. We calibrate through esp_adc's line-fitting scheme (eFuse
 *     Vref/two-point when burnt) and only fall back to the nominal 1100 mV Vref
 *     if the part has no calibration data.
 *   - The rail is noisy. Backlight PWM and Wi-Fi bursts move it tens of mV, so
 *     we take the MEDIAN of a burst (immune to a single spike, unlike a mean)
 *     and then smooth across reads.
 *   - Li-ion voltage is not linear in charge. A lookup curve maps mV -> %.
 *
 * A percentage is only shown when the reading is physically plausible for a
 * single cell. Outside that window we report -1 and the dashboard says "USB",
 * because a floating pin reading as "31%" is worse than admitting we cannot
 * tell. */
#define BAT_CHAN        ADC_CHANNEL_6    /* GPIO34 */
#define BAT_ATTEN       ADC_ATTEN_DB_12  /* ~0..3.1 V at the pin -> ~0..6.2 V at the cell */
#define BAT_DIVIDER     2                /* IO34 sees Vbat/2 */
#define BAT_SAMPLES     15               /* odd, so the median is a real sample */
#define BAT_VREF_NOMINAL 1100            /* only used on an uncalibrated part */
#define BAT_PLAUSIBLE_LO 2600            /* below a protected cell's cutoff -> not a battery */
#define BAT_PLAUSIBLE_HI 4600            /* above any single-cell charge voltage */
#define BAT_PERIOD_US   (5*1000*1000)    /* re-sample at most this often */

/* Trim for the divider's resistor tolerance, in per-mille (1000 = no change).
 * Raise it if the reported voltage reads low against a multimeter at the JP2
 * pads. Left at unity until there is a bench measurement to justify otherwise
 * -- an invented correction is worse than none. */
#define BAT_TRIM_PERMILLE 1000

static adc_oneshot_unit_handle_t g_adc;
static adc_cali_handle_t         g_cali;      /* NULL = no eFuse calibration */
static int      g_bat_mv;                     /* smoothed cell voltage, 0 = no reading yet */
static int64_t  g_bat_when;                   /* esp_timer stamp of that reading */

/* Discharge curve for a single Li-ion/LiPo cell under a light load, descending.
 * The flat middle is why a linear voltage->percent map feels so wrong on these
 * cells: 3.84 V to 3.80 V is a tenth of the pack. */
static const struct { short mv, pct; } BAT_CURVE[] = {
    {4200,100},{4150, 95},{4110, 90},{4080, 85},{4020, 80},{3980, 75},
    {3950, 70},{3910, 65},{3870, 60},{3850, 55},{3840, 50},{3820, 45},
    {3800, 40},{3790, 35},{3770, 30},{3750, 25},{3730, 20},{3710, 16},
    {3690, 13},{3610,  9},{3270,  0},
};

static int cmp_int(const void *a, const void *b){
    int x = *(const int*)a, y = *(const int*)b;
    return (x > y) - (x < y);
}

static void bat_init(void){
    adc_oneshot_unit_init_cfg_t u = { .unit_id = ADC_UNIT_1 };
    if(adc_oneshot_new_unit(&u, &g_adc) != ESP_OK){
        ESP_LOGW(TAG, "battery: ADC1 unavailable -- gauge disabled");
        g_adc = NULL; return;
    }
    adc_oneshot_chan_cfg_t c = { .atten = BAT_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if(adc_oneshot_config_channel(g_adc, BAT_CHAN, &c) != ESP_OK){
        ESP_LOGW(TAG, "battery: channel config failed -- gauge disabled");
        adc_oneshot_del_unit(g_adc); g_adc = NULL; return;
    }
    adc_cali_line_fitting_config_t cal = {
        .unit_id      = ADC_UNIT_1,
        .atten        = BAT_ATTEN,
        .bitwidth     = ADC_BITWIDTH_DEFAULT,
        .default_vref = BAT_VREF_NOMINAL,
    };
    if(adc_cali_create_scheme_line_fitting(&cal, &g_cali) != ESP_OK){
        g_cali = NULL;
        ESP_LOGW(TAG, "battery: no eFuse ADC calibration -- readings are approximate");
    }
}

/* one burst -> median cell millivolts, or -1 if the ADC is not usable. */
static int bat_sample_mv(void){
    if(!g_adc) return -1;
    int mv[BAT_SAMPLES], n = 0;
    for(int i = 0; i < BAT_SAMPLES; i++){
        int raw = 0;
        if(adc_oneshot_read(g_adc, BAT_CHAN, &raw) != ESP_OK) continue;
        int one = 0;
        if(g_cali){
            if(adc_cali_raw_to_voltage(g_cali, raw, &one) != ESP_OK) continue;
        } else {
            /* uncalibrated: the nominal 12 dB full scale. Approximate on purpose. */
            one = raw * 3100 / 4095;
        }
        mv[n++] = one;
    }
    if(n == 0) return -1;
    qsort(mv, n, sizeof mv[0], cmp_int);
    int at_pin = mv[n/2];
    return at_pin * BAT_DIVIDER * BAT_TRIM_PERMILLE / 1000;
}

int power_battery_mv(void){
    if(!g_adc) return -1;
    int64_t now = esp_timer_get_time();
    if(g_bat_mv && now - g_bat_when < BAT_PERIOD_US) return g_bat_mv;

    int mv = bat_sample_mv();
    if(mv < 0) return g_bat_mv ? g_bat_mv : -1;
    g_bat_when = now;
    /* Smooth across reads (3:1 toward the running value). The cell moves over
     * hours; the noise moves over milliseconds. */
    g_bat_mv = g_bat_mv ? (g_bat_mv * 3 + mv) / 4 : mv;
    return g_bat_mv;
}

int power_battery_pct(void){
    int mv = power_battery_mv();
    if(mv < BAT_PLAUSIBLE_LO || mv > BAT_PLAUSIBLE_HI) return -1;   /* no cell fitted, or USB-only */

    const int N = (int)(sizeof BAT_CURVE / sizeof BAT_CURVE[0]);
    if(mv >= BAT_CURVE[0].mv)     return 100;
    if(mv <= BAT_CURVE[N-1].mv)   return 0;
    for(int i = 1; i < N; i++){
        if(mv >= BAT_CURVE[i].mv){
            int hi = BAT_CURVE[i-1].mv, lo = BAT_CURVE[i].mv;
            int ph = BAT_CURVE[i-1].pct, pl = BAT_CURVE[i].pct;
            return pl + (mv - lo) * (ph - pl) / (hi - lo);
        }
    }
    return 0;
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

    bat_init();
    { int mv = power_battery_mv(), pct = power_battery_pct();
      if(mv < 0)       ESP_LOGI(TAG, "battery: gauge unavailable");
      else if(pct < 0) ESP_LOGI(TAG, "battery: %d mV -- outside single-cell range, reporting USB", mv);
      else             ESP_LOGI(TAG, "battery: %d mV -> %d%% (%d mV at IO34, cali=%s)",
                                mv, pct, mv / BAT_DIVIDER, g_cali ? "eFuse" : "nominal"); }

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
    res_accrue();                          /* bank the interval under the OLD state */
    g_off = !on;
    apply_duty(on ? g_bright : 0);
}

int power_screen_off(void){ return g_off; }

/* ========================== drain log (see power.h) ========================= */
#define POWER_LOG    "/sdcard/power.log"
#define POWER_LOG_S  300                  /* one sample every 5 minutes */

static int64_t  s_res_mark;               /* esp_timer at the last state change */
static int64_t  s_lit_us, s_dark_us;      /* cumulative residency this boot */
static int64_t  s_lit_at_log, s_dark_at_log;
static unsigned s_syncs, s_syncs_at_log;
static int64_t  s_last_log_us;
static int      s_first_mv = -1, s_first_pct = -1;
static unsigned s_samples;
static int      s_logging;

/* Bank the time since the last transition under whichever state was in force.
 * Called on every backlight change and before every sample, so the split is
 * exact rather than sampled -- a screen lit for 40 s between two 5-minute
 * samples is 40 s, not a coin flip. */
static void res_accrue(void){
    int64_t now = esp_timer_get_time();
    if(s_res_mark){
        if(g_off) s_dark_us += now - s_res_mark;
        else      s_lit_us  += now - s_res_mark;
    }
    s_res_mark = now;
}

void power_note_sync(void){ s_syncs++; }

/* One line. `note` distinguishes the boot marker from an ordinary interval --
 * without it a reboot looks like a discontinuity in the voltage series with no
 * explanation, and those are exactly the samples that must not be regressed on. */
static void log_line(const char *note){
    res_accrue();
    int mv = power_battery_mv(), pct = power_battery_pct();

    int64_t now  = esp_timer_get_time();
    long lit  = (long)((s_lit_us  - s_lit_at_log ) / 1000000);
    long dark = (long)((s_dark_us - s_dark_at_log) / 1000000);
    s_lit_at_log = s_lit_us; s_dark_at_log = s_dark_us;
    unsigned syncs = s_syncs - s_syncs_at_log; s_syncs_at_log = s_syncs;
    s_last_log_us = now;

    time_t t = 0; time(&t); struct tm ti; localtime_r(&t, &ti);

    /* The header goes in only when the file is new, so an existing log is
     * appended to across power cycles -- the run being measured is longer than
     * any one boot. */
    int fresh = 0;
    FILE *probe = fopen(POWER_LOG, "r");
    if(probe) fclose(probe); else fresh = 1;

    FILE *f = fopen(POWER_LOG, "a");
    if(!f){ if(!s_logging) ESP_LOGW(TAG, "power.log: cannot open (no card?)"); return; }
    if(fresh)
        fprintf(f, "# epoch,iso,mv,pct,uptime_s,lit_s,dark_s,syncs,note\n"
                   "# lit_s/dark_s/syncs are for THIS interval; uptime_s is cumulative.\n");
    fprintf(f, "%lld,%04d-%02d-%02dT%02d:%02d:%02d,%d,%d,%ld,%ld,%ld,%u,%s\n",
            (long long)t, ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday,
            ti.tm_hour, ti.tm_min, ti.tm_sec,
            mv, pct, (long)(now / 1000000), lit, dark, syncs, note);
    fclose(f);
    s_samples++;
}

void power_log_start(void){
    if(s_logging) return;
    s_logging  = 1;
    s_res_mark = esp_timer_get_time();
    s_first_mv = power_battery_mv();
    s_first_pct = power_battery_pct();
    log_line("boot");
    ESP_LOGI(TAG, "power.log: logging every %d s (opening reading %d mV / %d%%)",
             POWER_LOG_S, s_first_mv, s_first_pct);
}

void power_log_mark(const char *note){
    if(!s_logging) return;
    char safe[16]; int j = 0;
    for(int i = 0; note && note[i] && j < (int)sizeof safe - 1; i++)
        if(note[i] != ',' && note[i] != '\n') safe[j++] = note[i];
    safe[j] = 0;
    log_line(j ? safe : "mark");
}

void power_log_tick(void){
    if(!s_logging) return;
    if(esp_timer_get_time() - s_last_log_us < (int64_t)POWER_LOG_S * 1000000) return;
    log_line("");
}

void power_stats(PowerStats *st){
    if(!st) return;
    res_accrue();
    st->mv        = power_battery_mv();
    st->pct       = power_battery_pct();
    st->first_mv  = s_first_mv;
    st->first_pct = s_first_pct;
    st->up_s      = (long)(esp_timer_get_time() / 1000000);
    st->lit_s     = (long)(s_lit_us  / 1000000);
    st->dark_s    = (long)(s_dark_us / 1000000);
    st->syncs     = s_syncs;
    st->samples   = s_samples;
}
