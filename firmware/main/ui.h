/* ui.h -- the Palm-style app shell (LVGL). */
#ifndef UI_H
#define UI_H

/* build the initial UI (launcher). call after lvgl_port_init(). */
void ui_init(void);

/* raise the lock-screen dashboard (idempotent). Shown at boot and re-raised by the
 * port layer whenever the screen wakes from the idle blank. Swipe up dismisses it. */
void ui_show_lock(void);

#endif
