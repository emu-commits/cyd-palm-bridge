/* host_main.c -- native headless frontend for the simulator.
 *
 * Boots the REAL firmware UI (ui_init from firmware/main/ui.c) against the sim
 * port and drives it from a tiny stdin script, dumping screenshots as PPM (P6).
 * This is both the local development loop (render -> look at the PNG -> choose
 * the next tap) and the CI smoke gate (scripted run must exit 0).
 *
 * Script commands (one per line; '#' comments):
 *   t <ms>          advance simulated time
 *   w <ms>          wait <ms> of REAL wall-clock time (still pumping LVGL), for the
 *                   few behaviours tied to time(NULL) rather than LVGL ticks --
 *                   notably the games' play clocks (firmware/main/playclock.h)
 *   d <x> <y>       pointer down at (x,y)
 *   m <x> <y>       pointer move (while down; use for Graffiti strokes)
 *   u               pointer up
 *   c <x> <y>       click = down, 80 ms, up, 200 ms
 *   s <name>        screenshot -> <shotdir>/<name>.ppm
 *   q               quit (implicit at EOF)
 *
 * Usage: sim_host [shotdir] < script      (shotdir default "build/shots")
 * Needs /sdcard to exist and be writable (the firmware data layer's SD root):
 * locally `mkdir /sdcard`; on a CI runner `sudo mkdir -p /sdcard && sudo chmod 777`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "lvgl.h"
#include "sim_port.h"
#include "sim_heap.h"
#include "ui.h"
#include "data.h"
#include "appcfg.h"
#include "clock.h"

static const char *s_shotdir = "build/shots";

/* ---- the LVGL object pool, watched across the whole run --------------------
 * THE POOL IS THIS PROJECT'S OLDEST AND MOST REPEATED FAILURE. The record list
 * hit it, the zone picker hit it at ~24 buttons, the Preferences list hit it at
 * three extra rows, the brightness popup hit it hardest because a bar's draw
 * layer fails as a WDT FREEZE rather than an error -- and most recently a
 * 27-row lv_list in the event time picker crashed the moment it was opened.
 *
 * Every one of those was found by a person tapping the glass, because the gate
 * only noticed the pool when exhaustion happened to be fatal DURING the walk.
 * A screen that leaves 200 bytes free passes and ships. So the run now reads
 * lv_mem_monitor() at every screenshot, remembers the worst, and fails if it
 * ever drops under a floor -- which turns "it crashed on the device, sometimes"
 * into "this shot took the pool to N bytes", with the screen's own name on it.
 *
 * The floor is deliberately generous: it is not a budget to spend down to, it
 * is a tripwire for a screen that has gone structurally wrong (a per-row widget
 * where a table belongs). Normal screens here sit 11-17 KB free. */
#define POOL_FLOOR 3072
static size_t s_pool_min = (size_t)-1;
static char   s_pool_min_shot[128] = "(none)";
static int    s_pool_bad;

/* Pump the UI for `ms` of REAL time. `t` advances only LVGL's simulated tick, so
 * anything reading time(NULL) -- the game play clocks -- stands still under it.
 * This burns actual seconds, so scripts should use it sparingly. */
static void wall_wait(int ms){
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for(;;){
        clock_gettime(CLOCK_MONOTONIC, &now);
        long el = (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_nsec - t0.tv_nsec) / 1000000L;
        if(el >= ms) return;
        sim_step(20);
        struct timespec nap = { 0, 15 * 1000 * 1000 };   /* 15 ms, so we don't spin */
        nanosleep(&nap, NULL);
    }
}

static int shot(const char *name){
    char path[256];
    snprintf(path, sizeof path, "%s/%s.ppm", s_shotdir, name);
    FILE *f = fopen(path, "wb");
    if(!f){ fprintf(stderr, "shot: cannot write %s\n", path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", SIM_W, SIM_H);
    const uint8_t *fb = sim_fb_ptr();
    for(int i = 0; i < SIM_W * SIM_H; i++) fwrite(fb + i * 4, 1, 3, f);  /* drop A */
    fclose(f);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    if(mon.free_size < s_pool_min){
        s_pool_min = mon.free_size;
        snprintf(s_pool_min_shot, sizeof s_pool_min_shot, "%s", name);
    }
    if(mon.free_size < POOL_FLOOR){
        fprintf(stderr, "POOL FLOOR: %s left only %u bytes free (floor %u, "
                        "largest block %u) -- a screen this close to the ceiling "
                        "freezes the device\n",
                name, (unsigned)mon.free_size, (unsigned)POOL_FLOOR,
                (unsigned)mon.free_biggest_size);
        s_pool_bad = 1;
    }
    fprintf(stderr, "shot: %s | pool free=%u\n", path, (unsigned)mon.free_size);
    return 0;
}

int main(int argc, char **argv){
    if(argc > 1) s_shotdir = argv[1];
    mkdir(s_shotdir, 0777);

    /* sanity: the firmware data layer roots at /sdcard */
    struct stat st;
    if(stat("/sdcard", &st) != 0){
        fprintf(stderr, "ERROR: /sdcard does not exist -- create it first "
                        "(mkdir /sdcard, or sudo on a CI runner)\n");
        return 2;
    }

    /* mirror app_main's boot order (minus hardware): seed -> tz -> LVGL -> UI */
    data_seed_if_empty();
    clock_set_tz(appcfg()->timezone);
    sim_init();
    ui_init();
    sim_step(300);   /* let the first layout/draw settle */
    sim_heap_arm(SIM_HEAP_BUDGET);   /* device-like general-heap ceiling from here on */

    char line[256];
    int rc = 0;
    while(fgets(line, sizeof line, stdin)){
        int x, y, ms;
        char name[128];
        if(line[0] == '#' || line[0] == '\n') continue;
        if(sscanf(line, "t %d", &ms) == 1)            sim_step(ms);
        else if(sscanf(line, "w %d", &ms) == 1)       wall_wait(ms);
        else if(sscanf(line, "d %d %d", &x, &y) == 2){ sim_touch(x, y, 1); sim_step(30); }
        else if(sscanf(line, "m %d %d", &x, &y) == 2){ sim_touch(x, y, 1); sim_step(15); }
        else if(line[0] == 'u')                       { sim_touch(0, 0, 0); sim_step(50); }
        else if(sscanf(line, "c %d %d", &x, &y) == 2){
            sim_touch(x, y, 1); sim_step(80);
            sim_touch(x, y, 0); sim_step(200);
        }
        else if(sscanf(line, "s %127s", name) == 1)   { if(shot(name)) rc = 1; }
        else if(line[0] == 'q') break;
        else { fprintf(stderr, "script: bad line: %s", line); rc = 1; }
    }
    fprintf(stderr, "sim_host: done (rc=%d) | heap used=%zu peak=%zu of %u budget\n",
            rc, sim_heap_used(), sim_heap_peak(), (unsigned)SIM_HEAP_BUDGET);
    fprintf(stderr, "sim_host: lvgl pool low-water %u bytes, at \"%s\" (floor %u)\n",
            (unsigned)(s_pool_min == (size_t)-1 ? 0 : s_pool_min),
            s_pool_min_shot, (unsigned)POOL_FLOOR);
    if(s_pool_bad) rc = 1;
    return rc;
}
