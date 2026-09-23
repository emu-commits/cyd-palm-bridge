/* appcfg.c -- see appcfg.h. Defaults, then config.ini, then the passwords from
 * the device's own flash (secretstore.h).
 *
 * THERE IS NO COMPILE-TIME SEED ANY MORE. A gitignored firmware/main/secrets.h
 * used to be compiled in as the starting config, and it was a liability twice
 * over: it put a developer's real Wi-Fi and Apple passwords into any binary they
 * built -- the simulator's included, for months, because a quoted include
 * resolves beside the including file before any -I path -- and it made it
 * possible to hand someone a firmware image with credentials inside it. Every
 * value it held can be set on the device now (Settings), so it is gone.
 * `make -C sim nosecrets` still guards the property it protected. */
#include "appcfg.h"
#include "clock.h"        /* the built-in city table: see resolve_loc_auto */
#include "secretstore.h"  /* the passwords, off the card                   */
#include "esp_log.h"
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

/* The passwords. config.ini is on a removable card, so the device keeps them in
 * its own flash (secretstore.h) and the file keeps everything else.
 *
 * On LOAD, a password that IS in the file -- a card from before this, or one
 * typed on a computer -- is moved into the store; a field the file leaves
 * empty is filled from it. Returns how many were moved, so the caller can
 * rewrite the file without them. A move that fails leaves the password where
 * it was: better in the file than lost. */
static int secrets_settle(Config *c, int *failed){
    int moved = 0; char k[12];
    *failed = 0;
    for(int i = 0; i < CFG_WIFI_N; i++){
        if(!c->wifi[i].ssid[0]) continue;
        secret_wifi_key(c->wifi[i].ssid, k);
        if(c->wifi[i].pass[0]){ if(secret_set(k, c->wifi[i].pass) == 0) moved++; else (*failed)++; }
        else secret_get(k, c->wifi[i].pass, sizeof c->wifi[i].pass);
    }
    if(c->dav_pass[0]){ if(secret_set("dav", c->dav_pass) == 0) moved++; else (*failed)++; }
    else secret_get("dav", c->dav_pass, sizeof c->dav_pass);
    return moved;
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
    g_from_sd = (config_load(CFG_PATH, &g_cfg) == 0);
    int failed = 0;
    int moved = secrets_settle(&g_cfg, &failed);
    resolve_loc_auto(&g_cfg);
    g_loaded  = 1;
    /* a password was found on the card: take it off again */
    if(moved && !failed && g_from_sd) appcfg_save();

    /* One line that says where the config came from and what it holds --
     * COUNTS, never values. It is how a bench check can tell "no password
     * saved" from "wrong password" without anyone opening config.ini. */
    int nets = 0, keyed = 0;
    for(int i = 0; i < CFG_WIFI_N; i++)
        if(g_cfg.wifi[i].ssid[0]){ nets++; if(g_cfg.wifi[i].pass[0]) keyed++; }
    ESP_LOGI("appcfg", "config: %s, %d Wi-Fi network(s) (%d with a password), "
             "account %s, password %s%s", g_from_sd ? "config.ini" : "defaults (no config.ini)",
             nets, keyed, g_cfg.dav_user[0] ? "set" : "unset",
             g_cfg.dav_pass[0] ? "held" : "none",
             moved ? (failed ? "; could NOT move them off the card" : "; moved off the card") : "");
}

const Config* appcfg(void){ if(!g_loaded) appcfg_load(); return &g_cfg; }
Config*       appcfg_mut(void){ if(!g_loaded) appcfg_load(); return &g_cfg; }
int           appcfg_from_sd(void){ if(!g_loaded) appcfg_load(); return g_from_sd; }

/* The passwords go to the store and the file gets a copy with them blanked.
 * If the store refuses any of them, the file keeps them all -- a password on
 * the card is a privacy problem; a password nowhere is a device that cannot
 * connect. */
int appcfg_save(void){
    if(!g_loaded) appcfg_load();
    int stored = 1; char k[12];
    for(int i = 0; i < CFG_WIFI_N; i++){
        if(!g_cfg.wifi[i].ssid[0]) continue;
        secret_wifi_key(g_cfg.wifi[i].ssid, k);
        if(secret_set(k, g_cfg.wifi[i].pass) != 0) stored = 0;
    }
    if(secret_set("dav", g_cfg.dav_pass) != 0) stored = 0;

    Config out = g_cfg;
    if(stored){
        for(int i = 0; i < CFG_WIFI_N; i++) out.wifi[i].pass[0] = 0;
        out.dav_pass[0] = 0;
    }
    int r = config_save(CFG_PATH, &out);
    if(r == 0) g_from_sd = 1;
    return r;
}
