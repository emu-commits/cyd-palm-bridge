/* appcfg.c -- see appcfg.h. Seeds from secrets.h, then overlays config.ini. */
#include "appcfg.h"
#include "clock.h"   /* the built-in city table: see resolve_loc_auto */
#ifndef SIM_NO_SECRETS
#include "secrets.h"      /* compile-time seed (also .example in the repo)     */
#endif
/* SIM_NO_SECRETS (set by sim/Makefile) omits the seed header entirely, so every
 * WIFI_SSID / DAV_PASS macro below is simply undefined and seed_from_secrets()
 * compiles away to nothing.
 *
 * It must be done HERE, by not including the file, rather than by pointing the
 * include path at a stub. sim/include/secrets.h was exactly that stub and it
 * never once got used: this file lives in firmware/main, and a QUOTED include
 * is resolved relative to the including file's own directory before any -I path
 * is searched -- so firmware/main/secrets.h shadowed the stub unconditionally.
 * The result was a simulator binary carrying a developer's real Wi-Fi password
 * and Apple app-specific password, with the SSID and Apple ID legible in the
 * smoke screenshots. CI never saw it, because secrets.h is gitignored and CI
 * has no such file -- which is precisely why it survived so long.
 *
 * `make -C sim nosecrets` now fails if a seeded credential reaches the config. */
#include <stdio.h>
#include <string.h>

/* Overridable so a gate can point it at a path that does not exist and see the
 * config as it arrives from the SEED alone, with no config.ini overlaying it.
 * That is the only way `nosecrets` can tell a seeded credential apart from one
 * the user legitimately saved to the card. */
#ifndef CFG_PATH
#define CFG_PATH "/sdcard/config.ini"
#endif

static Config g_cfg;
static int    g_loaded  = 0;
static int    g_from_sd = 0;

/* copy a compile-time secrets.h macro into a field, only if the macro exists.
 * An older secrets.h may not define the To Do / Address collections or the
 * CardDAV host -- those stay at config_defaults() / empty, which just means the
 * app is skipped until configured (exactly the old behaviour). */
static void seed_from_secrets(Config *c){
#ifdef WIFI_SSID
    snprintf(c->wifi[0].ssid, sizeof c->wifi[0].ssid, "%s", WIFI_SSID);
#endif
#ifdef WIFI_PASS
    snprintf(c->wifi[0].pass, sizeof c->wifi[0].pass, "%s", WIFI_PASS);
#endif
#ifdef DAV_USER
    snprintf(c->dav_user, sizeof c->dav_user, "%s", DAV_USER);
#endif
#ifdef DAV_PASS
    snprintf(c->dav_pass, sizeof c->dav_pass, "%s", DAV_PASS);
#endif
#ifdef DAV_BASE
    snprintf(c->dav_base, sizeof c->dav_base, "%s", DAV_BASE);
#endif
#ifdef DAV_CARD_BASE
    snprintf(c->dav_card_base, sizeof c->dav_card_base, "%s", DAV_CARD_BASE);
#endif
#ifdef SYNC_COLL
    snprintf(c->cal_coll, sizeof c->cal_coll, "%s", SYNC_COLL);
#endif
#ifdef SYNC_TODO_COLL
    snprintf(c->todo_coll, sizeof c->todo_coll, "%s", SYNC_TODO_COLL);
#endif
#ifdef SYNC_CARD_COLL
    snprintf(c->card_coll, sizeof c->card_coll, "%s", SYNC_CARD_COLL);
#endif
}

/* Settle `loc_auto` for a card that predates it (config.h explains the three
 * states). The question is "did a human type these coordinates, or did a picker
 * put them there", and there is exactly one piece of evidence: the pickers can
 * only ever write a built-in city's coordinates, verbatim. So an exact match
 * against the table means APPROXIMATE -- refine it -- and anything else means a
 * person chose those digits and nothing may touch them.
 *
 * A location that is simply absent is approximate too: there is nothing to
 * protect and everything to gain.
 *
 * The one case this gets wrong is a user who typed, by hand, a coordinate that
 * matches a built-in city to the digit. They get their location refined to the
 * same town they typed, which is the harmless direction to be wrong in. */
static void resolve_loc_auto(Config *c){
    if(c->loc_auto >= 0) return;                  /* the file said; believe it */
    if(!c->latitude[0] || !c->longitude[0]){ c->loc_auto = 1; return; }
    for(int i = 0; i < clock_zone_count(); i++){
        const char *lat = NULL, *lon = NULL;
        if(!clock_zone_latlon(i, &lat, &lon)) continue;
        if(!strcmp(lat, c->latitude) && !strcmp(lon, c->longitude)){
            c->loc_auto = 1;                      /* a picker wrote this */
            return;
        }
    }
    c->loc_auto = 0;                              /* somebody typed it */
}

void appcfg_load(void){
    config_defaults(&g_cfg);
    seed_from_secrets(&g_cfg);
    g_from_sd = (config_load(CFG_PATH, &g_cfg) == 0);
    resolve_loc_auto(&g_cfg);
    g_loaded  = 1;
}

const Config* appcfg(void){ if(!g_loaded) appcfg_load(); return &g_cfg; }
Config*       appcfg_mut(void){ if(!g_loaded) appcfg_load(); return &g_cfg; }
int           appcfg_from_sd(void){ if(!g_loaded) appcfg_load(); return g_from_sd; }

int appcfg_save(void){
    if(!g_loaded) appcfg_load();
    int r = config_save(CFG_PATH, &g_cfg);
    if(r == 0) g_from_sd = 1;
    return r;
}
