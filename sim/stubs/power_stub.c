/* power_stub.c -- simulator stand-in for firmware/main/power.c.
 * No backlight to PWM; brightness is remembered so the Preferences slider
 * behaves, and the screen is never considered blanked. */
#include "power.h"

static int g_bright = 80;

void power_init(void)              {}
void power_set_brightness(int pct) { if(pct < 0) pct = 0; if(pct > 100) pct = 100; g_bright = pct; }
void power_backlight(int on)       { (void)on; }
int  power_screen_off(void)        { return 0; }
