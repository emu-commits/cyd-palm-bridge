/* wasm_main.c -- browser frontend for the simulator (Emscripten).
 *
 * main() boots the Palm (runs once at module init); JS then drives frames:
 *   per animation frame:  _sim_frame(elapsed_ms)  -> LVGL timers + rendering
 *   blit:                 _sim_fb()               -> RGBA framebuffer pointer
 *   input:                _sim_pointer(x, y, down) from pointer events
 *
 * /sdcard lives in Emscripten's in-memory FS (MEMFS): the firmware data layer
 * seeds and edits demo PDBs there exactly as on the real card. Edits persist
 * for the page's lifetime (IDBFS persistence is a later nicety). */
#include <sys/stat.h>
#include <emscripten/emscripten.h>
#include "sim_port.h"
#include "ui.h"
#include "data.h"
#include "appcfg.h"
#include "clock.h"

EMSCRIPTEN_KEEPALIVE void sim_boot(void){
    mkdir("/sdcard", 0777);            /* MEMFS */
    data_seed_if_empty();
    clock_set_tz(appcfg()->timezone);
    sim_init();
    ui_init();
    sim_step(300);                     /* settle the first layout/draw */
}

EMSCRIPTEN_KEEPALIVE void sim_frame(int elapsed_ms){
    if(elapsed_ms < 1)   elapsed_ms = 1;
    if(elapsed_ms > 100) elapsed_ms = 100;   /* clamp after a backgrounded tab */
    sim_step(elapsed_ms);
}

EMSCRIPTEN_KEEPALIVE void sim_pointer(int x, int y, int down){
    sim_touch(x, y, down);
}

EMSCRIPTEN_KEEPALIVE const unsigned char *sim_fb(void){
    return sim_fb_ptr();
}

int main(void){
    sim_boot();
    return 0;    /* EXIT_RUNTIME=0: the runtime stays alive for JS-driven frames */
}
