/* appcfg.c -- see appcfg.h. Seeds from secrets.h, then overlays config.ini. */
#include "appcfg.h"
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
    snprintf(c->wifi_ssid, sizeof c->wifi_ssid, "%s", WIFI_SSID);
#endif
#ifdef WIFI_PASS
    snprintf(c->wifi_pass, sizeof c->wifi_pass, "%s", WIFI_PASS);
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

void appcfg_load(void){
    config_defaults(&g_cfg);
    seed_from_secrets(&g_cfg);
    g_from_sd = (config_load(CFG_PATH, &g_cfg) == 0);
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
