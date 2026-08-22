/* power_stub.c -- simulator stand-in for firmware/main/power.c.
 * No backlight to PWM; brightness is remembered so the Preferences slider
 * behaves, and the screen is never considered blanked. */
#include "power.h"

static int g_bright = 80;

void power_init(void)              {}
void power_set_brightness(int pct) { if(pct < 0) pct = 0; if(pct > 100) pct = 100; g_bright = pct; }
void power_backlight(int on)       { (void)on; }
int  power_screen_off(void)        { return 0; }
/* the sim has no battery -- report a sample level so the dashboard tile renders.
 * 3.85 V is the matching point on the curve power.c uses on the device. */
int  power_battery_pct(void)       { return 72; }
int  power_battery_mv(void)        { return 3850; }
