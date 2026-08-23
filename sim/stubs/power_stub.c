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

/* The drain log measures a real cell over real hours; there is nothing for the
 * sim to sample. The Power screen still has to RENDER, though -- its layout and
 * its "no drain measured yet" wording are exactly the kind of thing worth seeing
 * without a device -- so the stats come back as a plausible early run. */
void power_log_start(void)             {}
void power_log_tick(void)              {}
void power_log_mark(const char *note)  { (void)note; }
void power_note_sync(void)             {}
void power_stats(PowerStats *st){
    if(!st) return;
    st->mv = 3850; st->pct = 72;
    st->first_mv = 4176; st->first_pct = 97;
    st->up_s = 5*3600 + 12*60;
    st->lit_s = 41*60; st->dark_s = st->up_s - st->lit_s;
    st->syncs = 3; st->samples = 63;
}
