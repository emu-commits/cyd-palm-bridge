/* host_main.c -- native headless frontend for the simulator.
 *
 * Boots the REAL firmware UI (ui_init from firmware/main/ui.c) against the sim
 * port and drives it from a tiny stdin script, dumping screenshots as PPM (P6).
 * This is both the local development loop (render -> look at the PNG -> choose
 * the next tap) and the CI smoke gate (scripted run must exit 0).
 *
 * Script commands (one per line; '#' comments):
 *   t <ms>          advance simulated time
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
#include "sim_port.h"
#include "ui.h"
#include "data.h"
#include "appcfg.h"
#include "clock.h"

static const char *s_shotdir = "build/shots";

static int shot(const char *name){
    char path[256];
    snprintf(path, sizeof path, "%s/%s.ppm", s_shotdir, name);
    FILE *f = fopen(path, "wb");
    if(!f){ fprintf(stderr, "shot: cannot write %s\n", path); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", SIM_W, SIM_H);
    const uint8_t *fb = sim_fb_ptr();
    for(int i = 0; i < SIM_W * SIM_H; i++) fwrite(fb + i * 4, 1, 3, f);  /* drop A */
    fclose(f);
    fprintf(stderr, "shot: %s\n", path);
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

    char line[256];
    int rc = 0;
    while(fgets(line, sizeof line, stdin)){
        int x, y, ms;
        char name[128];
        if(line[0] == '#' || line[0] == '\n') continue;
        if(sscanf(line, "t %d", &ms) == 1)            sim_step(ms);
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
    fprintf(stderr, "sim_host: done (rc=%d)\n", rc);
    return rc;
}
