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
/* 1 while a screen of discrete keys (the Calculator, the phone keypad) is up:
 * the input layer may then split a jumping press into two taps (tapsplit.h). */
int  ui_discrete_taps(void);

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
 * Simulator only: the firmware never defines UI_DEVTOOLS. */
void ui_test_type(const char *text);
#endif

#endif
