/* power.h -- backlight (PWM) + idle screen-off + automatic light-sleep.
 *
 * The TFT backlight LED is the single largest current draw on the CYD, so the
 * biggest battery win is dimming it to the configured brightness and turning it
 * off after an idle timeout (touch to wake). On top of that, esp_pm's automatic
 * light-sleep drops the SoC into low power during the idle delays between LVGL
 * frames. Both are driven from config.ini (brightness, backlight_sec). */
#ifndef POWER_H
#define POWER_H

/* configure the LEDC PWM backlight (on PIN_BL) + esp_pm light-sleep. Reads the
 * initial brightness from appcfg(). Call once after the SD/config is up. */
void power_init(void);

/* set backlight brightness 0..100 (persists as the "on" level). */
void power_set_brightness(int pct);

/* turn the backlight fully off (0) or back to the current brightness. Used by
 * the idle screen-off; does not change the stored brightness. */
void power_backlight(int on);

/* 1 if the screen is currently blanked by the idle timeout. */
int  power_screen_off(void);

/* battery charge estimate 0..100, or -1 if there is no usable reading -- no cell
 * on the JP2 seat, or a voltage outside the single-cell range. The dashboard shows
 * "USB" for -1 rather than inventing a percentage.
 *
 * The board reads the cell on ADC1 channel 6 (GPIO34) through a 2:1 divider. The
 * percentage comes off a Li-ion discharge curve, not a linear voltage map: these
 * cells sit near 3.8 V for most of their charge, so linear scaling reads wrong by
 * tens of percent through the middle of the range. */
int  power_battery_pct(void);

/* raw cell voltage in millivolts, or -1 if the gauge is unavailable. Exposed for
 * calibration: compare it against a multimeter at the JP2 pads and trim
 * BAT_TRIM_PERMILLE in power.c if the divider's resistors are off tolerance. */
int  power_battery_mv(void);

/* ---- drain log -----------------------------------------------------------
 * The experiment this exists for: how long does the device last on a cell, and
 * where does the charge actually go? That question cannot be answered over
 * serial, because attaching USB is what removes the battery from the circuit --
 * the TP4054 holds the rail at charge voltage and the discharge stops. So the
 * readings go to /sdcard/power.log and are also readable on-device (Menu >
 * Options > Power).
 *
 * A voltage series alone would not answer it either: a sample that dropped 40 mV
 * says nothing unless you know whether the screen was lit for that interval. So
 * every line carries the RESIDENCY of the interval it covers -- seconds lit,
 * seconds dark, syncs run -- which is what lets a drain be attributed to a cause
 * rather than just plotted. */

/* begin logging: writes the boot line and starts residency accounting. Call once
 * after the SD card is mounted. Safe to call with no card (the writes just fail). */
void power_log_start(void);

/* append one sample if the interval has elapsed. Drive from a UI timer -- it does
 * SD I/O and must not run on the small esp_timer task stack. */
void power_log_tick(void);

/* force a sample now, tagged with `note` (<=15 chars, no commas). This is how a
 * point in a discharge run gets a name -- "unplugged", "wifi off" -- so a step in
 * the series can be read back as a cause instead of a mystery. */
void power_log_mark(const char *note);

/* count a HotSync against the current interval (radio + SD are the expensive
 * parts of the budget, and they need separating from screen-on time). */
void power_note_sync(void);

/* Everything the Power screen reports, gathered in one call so the readings are
 * consistent with each other. Voltages are mV, -1 when the gauge is unavailable;
 * *_s are seconds. `first_*` is the opening reading of this power-up, which is
 * what a drain rate is measured against. */
typedef struct {
    int   mv, pct;             /* now                                        */
    int   first_mv, first_pct; /* at power_log_start()                        */
    long  up_s;                /* uptime                                      */
    long  lit_s, dark_s;       /* cumulative screen-on / screen-off this boot  */
    unsigned syncs;            /* HotSyncs this boot                          */
    unsigned samples;          /* lines appended to power.log this boot        */
} PowerStats;
void power_stats(PowerStats *st);

#endif
