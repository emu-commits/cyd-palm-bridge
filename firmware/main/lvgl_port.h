/* lvgl_port.h -- bind LVGL to the CYD display (display.c) + touch (touch.c). */
#ifndef LVGL_PORT_H
#define LVGL_PORT_H

/* init LVGL, register the display flush + touch input + tick. call once. */
void lvgl_port_init(void);

/* run the LVGL handler loop forever (call from a task / app_main). */
void lvgl_port_run(void);

#endif
