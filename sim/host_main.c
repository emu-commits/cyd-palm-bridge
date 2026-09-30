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
 *   k <text>        type into the focused field (see below)
 *   K <hex>         the same, with the text given in hex -- for a PASSWORD, so
 *                   the script (and stdin's buffer) never holds it in plain
 *   X <hex>         secretscan: fail if the hex-decoded string appears anywhere
 *                   in this process's writable memory (see secret_scan)
 *   Y <hex>         the same, but skipping the malloc heap: for while a password
 *                   screen is OPEN, whose own buffer is on that heap. Anywhere
 *                   else (the LVGL pool is a static array, so .bss) is a leak
 *   P <who> <hex>   fail unless the store holds that password for <who>: `a` for
 *                   the account, or a Wi-Fi slot 1..4. Proves the typing landed,
 *                   so an X that finds nothing means "wiped", not "never typed"
 *   A <seconds>     move Study's clock on (ui_test_study_skew): reviews come
 *                   due hours after a lesson, and the tour can't wait for them
 *   B               remember the heap in use now...
 *   E <what>        ...and fail unless it's the same again: a screen that was
 *                   opened and left must give back every byte it took
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

/* ---- secretscan ----------------------------------------------------------
 * A password must not outlive its use anywhere in RAM: not in a live buffer,
 * not in freed heap, not in the LVGL pool, not in a dead stack frame. So this
 * reads every WRITABLE mapping of the process (/proc/self/maps: .data, .bss,
 * the heap, anonymous maps, every stack) and looks for the exact bytes. Freed
 * memory is not cleared by anything, which is the point: it finds what a
 * missing wipe leaves behind.
 *
 * PIECES COUNT, not just the whole password. Freeing a block overwrites its
 * first bytes with the allocator's free-list pointers (glibc's tcache writes
 * 16; the ESP32's heap does the same), so an unwiped buffer that was freed no
 * longer holds the password from its start -- only its tail. A whole-string
 * search passes that. So any SCAN_WIN consecutive bytes of the password is a
 * hit, and the gate's test passwords are long enough that a freed copy always
 * leaves more than that behind.
 *
 * The needle itself is the one legitimate copy, so its own bytes are skipped.
 * It prints WHERE a copy is (the mapping's name and offset), never the value.
 *
 * skip_heap leaves out glibc's [heap], for a scan while a password screen is
 * still open: its buffer (malloc'd) legitimately holds the password then, but
 * nothing else may -- above all not the LVGL pool, which is a static array. A
 * scan after the screen is gone cannot prove that on its own, because LVGL
 * grows a label's text in place and the next screen reuses the block, so one
 * stray copy is overwritten before anyone looks. */
#define SCAN_WIN 12
static int hexval(int c){
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
/* decode hex into out (NUL-terminated); returns the length, or -1 */
static int unhex(const char *h, char *out, int cap){
    int n = 0;
    while(h[0] && h[0] != '\n' && h[1]){
        int a = hexval(h[0]), b = hexval(h[1]);
        if(a < 0 || b < 0 || n + 1 >= cap) return -1;
        out[n++] = (char)(a * 16 + b);
        h += 2;
    }
    out[n] = 0;
    return n;
}
static void wipe(void *p, size_t n){ volatile char *v = p; while(n--) *v++ = 0; }

static int secret_scan(const char *hex, int skip_heap){
    char *needle = malloc(128);
    if(!needle) return 1;
    int n = unhex(hex, needle, 128);
    if(n < 3 * SCAN_WIN){
        fprintf(stderr, "secretscan: use a test password of at least %d bytes\n", 3 * SCAN_WIN);
        free(needle); return 1;
    }
    FILE *m = fopen("/proc/self/maps", "r");
    if(!m){ fprintf(stderr, "secretscan: no /proc/self/maps\n"); wipe(needle, 128); free(needle); return 1; }
    char ln[512];
    int hits = 0; size_t scanned = 0;
    while(fgets(ln, sizeof ln, m)){
        unsigned long lo, hi; char perm[8] = "", name[256] = "";
        if(sscanf(ln, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &lo, &hi, perm, name) < 3) continue;
        if(perm[0] != 'r' || perm[1] != 'w') continue;          /* writable only */
        if(strstr(name, "[vvar")) continue;
        if(skip_heap && !strcmp(name, "[heap]")) continue;
        const char *b = (const char *)lo, *e = (const char *)hi;
        scanned += (size_t)(hi - lo);
        for(const char *p = b; p + SCAN_WIN <= e; p++){
            if(p >= needle && p < needle + 128) continue;          /* the needle itself */
            int w = 0;
            for(; w + SCAN_WIN <= n; w++)
                if(p[0] == needle[w] && !memcmp(p, needle + w, SCAN_WIN)) break;
            if(w + SCAN_WIN > n) continue;
            fprintf(stderr, "secretscan: FOUND %d+ bytes of it in %s at +0x%lx\n",
                    SCAN_WIN, name[0] ? name : "(anonymous)", (unsigned long)(p - b));
            hits++;
            p += SCAN_WIN - 1;                                     /* one report per run */
        }
    }
    fclose(m);
    wipe(needle, 128);
    free(needle);
    fprintf(stderr, "secretscan: %d cop%s in %zu KB of writable memory\n",
            hits, hits == 1 ? "y" : "ies", scanned / 1024);
    return hits ? 1 : 0;
}

static int secret_expect(const char *arg){
    char who = arg[0];
    const char *h = arg + 1;
    while(*h == ' ') h++;
    char *want = malloc(128), *have = malloc(128);
    int ok = 0;
    if(want && have && unhex(h, want, 128) > 0){
        if(who == 'a') appcfg_dav_pass(have, 128);
        else if(who >= '1' && who <= '0' + CFG_WIFI_N) appcfg_wifi_pass(who - '1', have, 128);
        else have[0] = 0;
        ok = !strcmp(want, have);
    }
    fprintf(stderr, "secretscan: the store %s the expected password for %c\n",
            ok ? "holds" : "does NOT hold", who);
    if(want){ wipe(want, 128); free(want); }
    if(have){ wipe(have, 128); free(have); }
    return ok ? 0 : 1;
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
    fprintf(stderr, "shot: %s | pool free=%u | heap used=%zu\n", path, (unsigned)mon.free_size,
            sim_heap_used());
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
    size_t heap_mark = 0;
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
        /* k <text> -- write into the focused field, as Graffiti would. The
         * script can tap and drag but it could not WRITE, so any screen whose
         * behaviour depends on what is IN a field (the Address Look Up filter,
         * the To Do / Memo quick-add bars) could only ever be photographed
         * empty. Takes the rest of the line, spaces and all. */
        else if(line[0] == 'k' && (line[1] == ' ' || line[1] == '\n' || !line[1])){
            char *t = line[1] == ' ' ? line + 2 : NULL;
            if(t){ char *nl = strchr(t, '\n'); if(nl) *nl = 0; }
            ui_test_type(t);            /* NULL (a bare `k`) empties the field */
            sim_step(60);
        }
        /* L -- raise the lock screen over whatever is showing, exactly as the
         * port layer does when the screen sleeps. The lock's relationship to
         * the screens and overlays under it (R11: an open Calculator) was
         * otherwise untestable: the script has no way to let the device sleep. */
        else if(line[0] == 'L' && (line[1] == '\n' || !line[1])){ ui_show_lock(); sim_step(100); }
        /* K <hex>: type a password without the plain text ever being in the
         * script, stdin's buffer or this line -- one character at a time. */
        else if(line[0] == 'K' && line[1] == ' '){
            const char *h = line + 2;
            while(h[0] && h[0] != '\n' && h[1]){
                int a = hexval(h[0]), b = hexval(h[1]);
                if(a < 0 || b < 0){ fprintf(stderr, "script: bad hex: %s", line); rc = 1; break; }
                char one[2] = { (char)(a * 16 + b), 0 };
                ui_test_type(one);
                wipe(one, sizeof one);
                h += 2;
            }
            sim_step(60);
        }
        else if(line[0] == 'A' && line[1] == ' '){ ui_test_study_skew((int32_t)atol(line + 2)); sim_step(60); }
        else if(line[0] == 'B' && (line[1] == '\n' || !line[1])){
            heap_mark = sim_heap_used();
            fprintf(stderr, "heap: %zu bytes in use (marked)\n", heap_mark);
        }
        else if(line[0] == 'E' && line[1] == ' '){
            size_t now = sim_heap_used();
            fprintf(stderr, "heap: %zu bytes in use after %s (marked %zu)\n", now, line + 2, heap_mark);
            if(now != heap_mark){
                fprintf(stderr, "HEAP: %zd bytes not given back after %s\n", (ssize_t)(now - heap_mark), line + 2);
                rc = 1;
            }
        }
        else if(line[0] == 'X' && line[1] == ' '){ if(secret_scan(line + 2, 0)) rc = 1; }
        else if(line[0] == 'Y' && line[1] == ' '){ if(secret_scan(line + 2, 1)) rc = 1; }
        else if(line[0] == 'P' && line[1] == ' '){ if(secret_expect(line + 2)) rc = 1; }
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
