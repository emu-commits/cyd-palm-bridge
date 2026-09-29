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
#include <stdlib.h>
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
/* config.ini still holds a password the store would not take (see
 * file_only_secrets). Almost always 0, and then the file is never re-read. */
static int    g_file_secrets = 0;

/* The passwords. config.ini is on a removable card, so the device keeps them in
 * its own flash (secretstore.h) and the file keeps everything else.
 *
 * On LOAD, a password that IS in the file -- a card from before this, or one
 * typed on a computer -- is moved into the store. Returns how many were moved,
 * so the caller can rewrite the file without them. A move that fails leaves the
 * password where it was: better in the file than lost. */
static int secrets_settle(const ConfigSecrets *sec, int *failed){
    int moved = 0; char k[12];
    *failed = 0;
    for(int i = 0; i < CFG_WIFI_N; i++){
        if(!g_cfg.wifi[i].ssid[0] || !sec->wifi_pass[i][0]) continue;
        secret_wifi_key(g_cfg.wifi[i].ssid, k);
        if(secret_set(k, sec->wifi_pass[i]) == 0) moved++; else (*failed)++;
    }
    if(sec->dav_pass[0]){ if(secret_set("dav", sec->dav_pass) == 0) moved++; else (*failed)++; }
    return moved;
}

/* The passwords config.ini holds that the STORE does not: the ones it refused
 * at load. Mapped onto the current slots by SSID, since slots reorder. Heap,
 * for as long as the caller needs it; free it with secrets_free(). NULL when
 * there are none, which is the normal case and costs no file read. */
static void secrets_free(ConfigSecrets *s){
    if(s){ config_wipe(s, sizeof *s); free(s); }
}
static ConfigSecrets *file_only_secrets(void){
    if(!g_file_secrets) return NULL;
    Config *tmp = malloc(sizeof *tmp);
    ConfigSecrets *fs = calloc(1, sizeof *fs), *keep = calloc(1, sizeof *keep);
    int any = 0;
    if(tmp && fs && keep){
        config_defaults(tmp);
        if(config_load(CFG_PATH, tmp, fs) == 0){
            char k[12];
            for(int i = 0; i < CFG_WIFI_N; i++){
                if(!g_cfg.wifi[i].ssid[0]) continue;
                secret_wifi_key(g_cfg.wifi[i].ssid, k);
                if(secret_has(k)) continue;
                for(int j = 0; j < CFG_WIFI_N; j++)
                    if(fs->wifi_pass[j][0] && !strcmp(tmp->wifi[j].ssid, g_cfg.wifi[i].ssid)){
                        snprintf(keep->wifi_pass[i], sizeof keep->wifi_pass[i], "%s", fs->wifi_pass[j]);
                        any = 1;
                        break;
                    }
            }
            if(fs->dav_pass[0] && !secret_has("dav")){
                snprintf(keep->dav_pass, sizeof keep->dav_pass, "%s", fs->dav_pass);
                any = 1;
            }
        }
    }
    secrets_free(fs);
    free(tmp);
    if(!any){ secrets_free(keep); return NULL; }
    return keep;
}

/* has_pass for every slot and the account, from the store (and the file, in
 * the rare case above). Reads lengths, never values. */
static void refresh_flags(void){
    ConfigSecrets *fo = file_only_secrets();
    char k[12];
    for(int i = 0; i < CFG_WIFI_N; i++){
        WifiNet *n = &g_cfg.wifi[i];
        if(!n->ssid[0]){ n->has_pass = 0; continue; }
        secret_wifi_key(n->ssid, k);
        n->has_pass = secret_has(k) || (fo && fo->wifi_pass[i][0]);
    }
    g_cfg.dav_has_pass = secret_has("dav") || (fo && fo->dav_pass[0]);
    secrets_free(fo);
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
    /* The file's passwords, if it has any, pass through this heap block on
     * their way to the store, and it is wiped before anything else runs. */
    ConfigSecrets *sec = calloc(1, sizeof *sec);
    g_from_sd = (config_load(CFG_PATH, &g_cfg, sec) == 0);
    int failed = 0;
    int moved = sec ? secrets_settle(sec, &failed) : 0;
    secrets_free(sec);
    /* No block to read them into means they were skipped, not moved: leave the
     * file alone and look there if the store turns out not to have them. */
    g_file_secrets = failed > 0 || (!sec && g_from_sd);
    resolve_loc_auto(&g_cfg);
    g_loaded  = 1;
    refresh_flags();
    /* a password was found on the card: take it off again */
    if(moved && !failed && g_from_sd) appcfg_save();

    /* One line that says where the config came from and what it holds --
     * COUNTS, never values. It is how a bench check can tell "no password
     * saved" from "wrong password" without anyone opening config.ini. */
    int nets = 0, keyed = 0;
    for(int i = 0; i < CFG_WIFI_N; i++)
        if(g_cfg.wifi[i].ssid[0]){ nets++; if(g_cfg.wifi[i].has_pass) keyed++; }
    ESP_LOGI("appcfg", "config: %s, %d Wi-Fi network(s) (%d with a password), "
             "account %s, password %s%s", g_from_sd ? "config.ini" : "defaults (no config.ini)",
             nets, keyed, g_cfg.dav_user[0] ? "set" : "unset",
             g_cfg.dav_has_pass ? "held" : "none",
             moved ? (failed ? "; could NOT move them off the card" : "; moved off the card") : "");
}

const Config* appcfg(void){ if(!g_loaded) appcfg_load(); return &g_cfg; }
Config*       appcfg_mut(void){ if(!g_loaded) appcfg_load(); return &g_cfg; }
int           appcfg_from_sd(void){ if(!g_loaded) appcfg_load(); return g_from_sd; }

int appcfg_save(void){
    if(!g_loaded) appcfg_load();
    ConfigSecrets *keep = file_only_secrets();   /* NULL in the normal case */
    int r = config_save(CFG_PATH, &g_cfg, keep);
    secrets_free(keep);
    if(r == 0) g_from_sd = 1;
    return r;
}

/* ---- passwords ---------------------------------------------------------- */
int appcfg_wifi_pass(int slot, char *out, size_t cap){
    if(cap) out[0] = 0;
    if(!g_loaded) appcfg_load();
    if(slot < 0 || slot >= CFG_WIFI_N || !g_cfg.wifi[slot].ssid[0] || !cap) return 0;
    char k[12];
    secret_wifi_key(g_cfg.wifi[slot].ssid, k);
    if(secret_get(k, out, cap)) return 1;
    ConfigSecrets *fo = file_only_secrets();
    if(fo && fo->wifi_pass[slot][0]) snprintf(out, cap, "%s", fo->wifi_pass[slot]);
    secrets_free(fo);
    return out[0] != 0;
}

int appcfg_dav_pass(char *out, size_t cap){
    if(cap) out[0] = 0;
    if(!g_loaded) appcfg_load();
    if(!cap) return 0;
    if(secret_get("dav", out, cap)) return 1;
    ConfigSecrets *fo = file_only_secrets();
    if(fo && fo->dav_pass[0]) snprintf(out, cap, "%s", fo->dav_pass);
    secrets_free(fo);
    return out[0] != 0;
}

int appcfg_set_wifi_pass(int slot, const char *pass){
    if(!g_loaded) appcfg_load();
    if(slot < 0 || slot >= CFG_WIFI_N || !g_cfg.wifi[slot].ssid[0]) return -1;
    char k[12];
    secret_wifi_key(g_cfg.wifi[slot].ssid, k);
    int r = secret_set(k, pass);
    refresh_flags();
    return r;
}

int appcfg_set_dav_pass(const char *pass){
    if(!g_loaded) appcfg_load();
    int r = secret_set("dav", pass);
    refresh_flags();
    return r;
}

void appcfg_set_wifi_ssid(int slot, const char *ssid){
    if(!g_loaded) appcfg_load();
    if(slot < 0 || slot >= CFG_WIFI_N) return;
    WifiNet *n = &g_cfg.wifi[slot];
    char old[sizeof n->ssid];
    snprintf(old, sizeof old, "%s", n->ssid);
    snprintf(n->ssid, sizeof n->ssid, "%s", ssid ? ssid : "");
    /* The old network's password goes when no slot names that network any more:
     * forgetting a network should forget its password too. */
    if(old[0] && strcmp(old, n->ssid)){
        int used = 0;
        for(int i = 0; i < CFG_WIFI_N; i++) if(!strcmp(g_cfg.wifi[i].ssid, old)) used = 1;
        if(!used){ char k[12]; secret_wifi_key(old, k); secret_set(k, NULL); }
    }
    refresh_flags();
}
