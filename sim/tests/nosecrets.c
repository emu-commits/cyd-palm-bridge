/* nosecrets.c -- prove a simulator build carries NO compile-time credentials.
 *
 * The bug this gate exists for: sim/Makefile used to "shield" the simulator
 * from a developer's real firmware/main/secrets.h by putting sim/include ahead
 * of it on the include path. That cannot work. appcfg.c lives in firmware/main
 * and includes "secrets.h" in QUOTES, and C resolves a quoted include relative
 * to the including file's own directory BEFORE searching any -I path -- so the
 * real header shadowed the stub unconditionally. Simulator binaries were built
 * with a real Wi-Fi password and Apple app-specific password compiled in, and
 * the smoke screenshots rendered the SSID and Apple ID as plain text.
 *
 * It survived because it is invisible exactly where anyone would look for it:
 * secrets.h is gitignored, so CI has no such file and CI builds were always
 * clean. Only a developer's own machine was affected, and only in artefacts
 * nobody diffs.
 *
 * So the gate has to run where the file exists -- on a real machine, against a
 * real secrets.h -- and it has to fail there and nowhere else. It does: CFG_PATH
 * is pointed at a file that does not exist, so config.ini cannot overlay the
 * seed, and anything non-empty in these four fields can ONLY have come from a
 * compile-time seed. On a machine with no secrets.h it passes trivially, which
 * is correct: there is nothing to leak.
 *
 * NEVER PRINT A VALUE. On failure this reports the field's NAME and its LENGTH
 * and nothing else -- a gate that dumps the credential it caught has published
 * it to every CI log and terminal scrollback that ever runs it.
 */
#include <stdio.h>
#include <string.h>
#include "appcfg.h"

int main(void){
    appcfg_load();
    const Config *c = appcfg();

    const struct { const char *name; const char *val; } f[] = {
        { "dav_user",  c->dav_user  },
        { "dav_pass",  c->dav_pass  },
    };

    int bad = 0;
    for(unsigned i = 0; i < sizeof f / sizeof f[0]; i++){
        if(f[i].val[0]){
            printf("nosecrets: FAIL -- %s is seeded into this build (%d chars)\n",
                   f[i].name, (int)strlen(f[i].val));
            bad = 1;
        }
    }
    /* EVERY Wi-Fi slot, not just the first. The device remembers four networks
     * now, and a check that only looked at slot 1 would pass a build carrying
     * three real passwords -- which is precisely the shape of the leak this
     * gate was written for (the history is at the top of appcfg.c). */
    for(int i = 0; i < CFG_WIFI_N; i++){
        if(c->wifi[i].ssid[0] || c->wifi[i].pass[0]){
            printf("nosecrets: FAIL -- wifi slot %d is seeded into this build "
                   "(ssid %d chars, pass %d chars)\n", i + 1,
                   (int)strlen(c->wifi[i].ssid), (int)strlen(c->wifi[i].pass));
            bad = 1;
        }
    }
    if(bad){
        printf("nosecrets: a simulator build must never carry credentials.\n"
               "  Something is compiling them in again. appcfg.c used to seed\n"
               "  from a gitignored secrets.h; that path was removed on purpose\n"
               "  and must not come back.\n");
        return 1;
    }
    printf("nosecrets: OK -- no compile-time credentials in a sim build\n");
    return 0;
}
