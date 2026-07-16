/* wasm_main.c -- browser frontend for the simulator (Emscripten).
 *
 * Boot is driven by JS so the persisted SD card can be restored first:
 *   module init -> JS mounts IDBFS at /sdcard, FS.syncfs(true) restores it,
 *   then calls _sim_boot(). Per animation frame JS calls _sim_frame(ms) and
 *   blits the RGBA framebuffer at _sim_fb(). Input arrives via _sim_pointer.
 *
 * Persistence: /sdcard is an IDBFS mount (IndexedDB -- per-browser, per-user,
 * origin-isolated; other websites cannot read it). JS periodically calls
 * _sim_scrub_config() and then FS.syncfs(false): the scrub blanks the password
 * fields in /sdcard/config.ini before every persist, so credentials typed into
 * Preferences work for the session (in RAM) but are NEVER written to browser
 * storage. Records/prefs persist; secrets don't. (Sync is stubbed in the sim,
 * so stored credentials would be pure risk with zero benefit.)
 *
 * Memory model: the LVGL pool is exact device parity (24 KB, lv_conf.h) and
 * sim_boot arms the general-heap budget (sim_heap.h) so big allocations fail
 * like the hardware's ~140 KB interactive free heap. */
#include <sys/stat.h>
#include <emscripten/emscripten.h>
#include "sim_port.h"
#include "sim_heap.h"
#include "ui.h"
#include "data.h"
#include "appcfg.h"
#include "clock.h"
#include "config.h"

EMSCRIPTEN_KEEPALIVE void sim_boot(void){
    mkdir("/sdcard", 0777);            /* no-op if the IDBFS mount restored it */
    data_seed_if_empty();              /* seeds only PDBs that don't exist yet */
    clock_set_tz(appcfg()->timezone);
    sim_init();
    ui_init();
    sim_step(300);                     /* settle the first layout/draw */
    sim_heap_arm(SIM_HEAP_BUDGET);     /* boot done -> device-like heap ceiling */
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

/* Blank the secret fields of the ON-DISK config.ini (the in-memory session
 * config is untouched). JS calls this immediately before every FS.syncfs, so
 * IndexedDB never receives a password. */
EMSCRIPTEN_KEEPALIVE void sim_scrub_config(void){
    Config c;
    config_defaults(&c);
    if(config_load("/sdcard/config.ini", &c) != 0) return;   /* no file yet */
    if(!c.wifi_pass[0] && !c.dav_pass[0]) return;            /* already clean */
    c.wifi_pass[0] = 0;
    c.dav_pass[0]  = 0;
    config_save("/sdcard/config.ini", &c);
}

int main(void){
    return 0;    /* JS boots after the IDBFS restore; runtime stays alive */
}
