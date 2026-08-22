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

#endif
