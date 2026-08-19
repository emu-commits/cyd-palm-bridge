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

#endif
