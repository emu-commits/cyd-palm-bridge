/* sim_port.h -- the simulator's LVGL platform port (the lvgl_port.c of the sim).
 *
 * One platform-neutral core serves both frontends:
 *   - host_main.c  (native headless: scripted input, PPM screenshots, CI smoke)
 *   - wasm_main.c  (browser: JS drives sim_step per animation frame and blits
 *                   the RGBA framebuffer to a <canvas>)
 *
 * Display parity with the device: a 40-row RGB565 PARTIAL draw buffer exactly
 * like firmware/main/lvgl_port.c, flushed into a full RGBA framebuffer here.
 * Time is injected (sim_step advances the LVGL tick), so headless runs are
 * deterministic. Touch is injected via sim_touch(). */
#ifndef SIM_PORT_H
#define SIM_PORT_H
#include <stdint.h>

#define SIM_W 240
#define SIM_H 320

/* bring up LVGL: display (Palm mono theme, authentic fonts) + injected pointer.
 * Call once, before ui_init(). */
void sim_init(void);

/* advance simulated time by `ms`, running LVGL timers/rendering as it goes. */
void sim_step(int ms);

/* inject the pointer: down=1 press/drag at (x,y) in 240x320 space, down=0 lift. */
void sim_touch(int x, int y, int down);

/* the rendered screen: SIM_W*SIM_H RGBA8888 bytes, updated by every flush. */
const uint8_t *sim_fb_ptr(void);

#endif
