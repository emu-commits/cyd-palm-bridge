/* ui.h -- the Palm-style app shell (LVGL). */
#ifndef UI_H
#define UI_H

/* build the initial UI (launcher). call after lvgl_port_init(). */
void ui_init(void);

/* Raise the lock-screen dashboard (idempotent). Shown at boot, raised by the port
 * layer the moment the screen sleeps -- so it is already on the glass when the
 * backlight comes back, rather than sliding in over the last app a beat later --
 * and re-called on each wake to refresh it. No-ops while Coach owns the screen.
 * Swipe up dismisses it. */
void ui_show_lock(void);

/* 1 while the UI is driving the backlight itself (Coach's end-of-session flash).
 * The port layer's idle blank and wake-poll stand down for the duration, so a tap
 * during a dark phase is not mistaken for a wake. */
int  ui_owns_backlight(void);
/* 1 while a screen asks to stay lit (Critters on USB power): the port layer
 * doesn't blank it. On battery it dims and locks as usual. */
int  ui_keep_awake(void);
/* 1 while a screen of discrete keys (the Calculator, the phone keypad) is up:
 * the input layer may then split a jumping press into two taps (tapsplit.h). */
int  ui_discrete_taps(void);
/* the screen on show, by the name in its title bar ("Lock" for the lock
 * screen): lets the port's pool monitor say WHERE memory ran low. */
const char *ui_screen_name(void);

#ifdef UI_DEVTOOLS
/* Put text into the focused field, as if it had been written in Graffiti.
 * A NULL `text` empties the field instead, which the script needs in order to
 * leave a filter box the way it found it.
 *
 * The smoke script can tap and drag but it cannot WRITE, and a quick-add bar
 * whose whole point is "type, tap New, a row appears" is untestable without
 * this -- the gate could only ever photograph the empty bar and call it done.
 * Stroking the letters through the recogniser instead would be testing the
 * recogniser, which has its own gate.
 *
 * The simulator always defines UI_DEVTOOLS; the firmware only with
 * CONFIG_CYD_DEVTOOLS (menuconfig, off by default). */
void ui_test_type(const char *text);
/* Move Study's clock on by `seconds` (it keeps its own offset from the real
 * one), so the smoke tour can reach reviews that come due hours after a
 * lesson without waiting for them. */
#include <stdint.h>
void ui_test_study_skew(int32_t seconds);
/* Look for reminders over (now - back, now + ahead] at once, as a wake would
 * over the time the screen was dark: the tour can't wait for an alarm, and an
 * event made "today" may be either side of the clock when it runs. */
void ui_test_reminders(int32_t back, int32_t ahead);
#endif

#endif
