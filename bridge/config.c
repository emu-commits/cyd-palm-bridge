/* config.c -- runtime device config: parse/serialise a key=value file. See config.h.
 *
 * RAM: parsing is line-at-a-time (one bounded stack line buffer, no heap, no
 * whole-file load), and the Config struct is ~1 KB the caller owns. Safe against
 * a hand-edited file: unknown keys and malformed lines are skipped, every string
 * copy is length-bounded.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>   /* strcasecmp */
#include "config.h"
#include "safefile.h"      /* config.ini is replaced whole, never rewritten in place */

static void setstr(char *dst, int cap, const char *val){
    snprintf(dst, cap, "%s", val);
}

void config_defaults(Config *c){
    memset(c, 0, sizeof *c);
    setstr(c->dav_base,      sizeof c->dav_base,      "https://caldav.icloud.com");
    setstr(c->dav_card_base, sizeof c->dav_card_base, "https://contacts.icloud.com");
    setstr(c->world1,        sizeof c->world1,        "Europe/London");   /* lock-screen defaults */
    setstr(c->world2,        sizeof c->world2,        "Asia/Tokyo");
    c->brightness    = 80;
    c->backlight_sec = 30;     /* dim after 30 s idle */
    c->clock24       = 0;      /* 12-hour by default */
    c->policy        = CFG_POL_SERVER;
    c->loc_auto      = -1;     /* "the file has not said"; see config.h */
}

int config_policy_from_str(const char *s){
    if(s && !strcasecmp(s,"local")) return CFG_POL_LOCAL;
    if(s && !strcasecmp(s,"both"))  return CFG_POL_BOTH;
    return CFG_POL_SERVER;
}
const char *config_policy_to_str(int policy){
    if(policy==CFG_POL_LOCAL) return "local";
    if(policy==CFG_POL_BOTH)  return "both";
    return "server";
}

/* Cut an INLINE comment off a value: everything from a '#' that FOLLOWS
 * whitespace to the end of the line. Deliberately NOT any bare '#' -- a password
 * or an SSID may legitimately contain one ("P#ssw0rd" survives; "pass # note"
 * does not, which is the documented cost of the syntax). This exists because
 * config.ini.example ships `timezone = America/New_York   # empty = floating`
 * and the parser used to keep the comment as part of the value: the zone then
 * matched nothing and silently fell back to UTC, and the same line shape on
 * `dav_pass` silently appended a comment to the password. */
static void cut_comment(char *s){
    for(char *p = s; *p; p++)
        if(*p=='#' && p>s && (p[-1]==' '||p[-1]=='\t')){ *p = 0; return; }
}

/* trim leading/trailing ASCII whitespace in place; returns the start pointer. */
static char *trim(char *s){
    while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n') s++;
    char *e=s+strlen(s);
    while(e>s && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e=0;
    return s;
}

/* clamp an int to [lo,hi]. */
static int clampi(int v,int lo,int hi){ return v<lo?lo:v>hi?hi:v; }

/* The key names for Wi-Fi slot `i`. SLOT 0 KEEPS THE UNNUMBERED NAMES it has
 * always had -- `wifi_ssid` / `wifi_pass` -- so a card written before there were
 * four networks still loads, into the slot that is tried first. The rest are
 * suffixed with their 1-based number, which is what a human editing the file
 * would expect to see next to the unnumbered pair. */
static void wifi_keys(int i, char *ks, size_t nks, char *kp, size_t nkp){
    if(i == 0){ snprintf(ks,nks,"wifi_ssid"); snprintf(kp,nkp,"wifi_pass"); }
    else      { snprintf(ks,nks,"wifi_ssid%d",i+1); snprintf(kp,nkp,"wifi_pass%d",i+1); }
}

int config_wifi_promote(Config *c, int i){
    if(!c || i <= 0 || i >= CFG_WIFI_N) return 0;
    WifiNet t = c->wifi[i];
    for(int k = i; k > 0; k--) c->wifi[k] = c->wifi[k-1];
    c->wifi[0] = t;
    return 1;
}

static void apply(Config *c, const char *key, const char *val){
    for(int i = 0; i < CFG_WIFI_N; i++){
        char ks[16], kp[16];
        wifi_keys(i, ks, sizeof ks, kp, sizeof kp);
        if(!strcasecmp(key,ks)){ setstr(c->wifi[i].ssid, sizeof c->wifi[i].ssid, val); return; }
        if(!strcasecmp(key,kp)){ setstr(c->wifi[i].pass, sizeof c->wifi[i].pass, val); return; }
    }
    if(!strcasecmp(key,"dav_user"))       setstr(c->dav_user,      sizeof c->dav_user, val);
    else if(!strcasecmp(key,"dav_pass"))  setstr(c->dav_pass,      sizeof c->dav_pass, val);
    else if(!strcasecmp(key,"dav_base"))  setstr(c->dav_base,      sizeof c->dav_base, val);
    else if(!strcasecmp(key,"dav_card_base")) setstr(c->dav_card_base, sizeof c->dav_card_base, val);
    else if(!strcasecmp(key,"cal_coll"))  setstr(c->cal_coll,      sizeof c->cal_coll, val);
    else if(!strcasecmp(key,"todo_coll")) setstr(c->todo_coll,     sizeof c->todo_coll, val);
    else if(!strcasecmp(key,"card_coll")) setstr(c->card_coll,     sizeof c->card_coll, val);
    else if(!strcasecmp(key,"timezone"))  setstr(c->timezone,      sizeof c->timezone, val);
    else if(!strcasecmp(key,"loc_auto"))      c->loc_auto      = clampi(atoi(val),0,1);
    else if(!strcasecmp(key,"loc_name"))  setstr(c->loc_name,      sizeof c->loc_name, val);
    else if(!strcasecmp(key,"latitude"))  setstr(c->latitude,      sizeof c->latitude, val);
    else if(!strcasecmp(key,"longitude")) setstr(c->longitude,     sizeof c->longitude, val);
    else if(!strcasecmp(key,"owner"))     setstr(c->owner,         sizeof c->owner, val);
    else if(!strcasecmp(key,"world1"))    setstr(c->world1,        sizeof c->world1, val);
    else if(!strcasecmp(key,"world2"))    setstr(c->world2,        sizeof c->world2, val);
    else if(!strcasecmp(key,"brightness"))    c->brightness    = clampi(atoi(val),0,100);
    else if(!strcasecmp(key,"backlight_sec")) c->backlight_sec = clampi(atoi(val),0,3600);
    else if(!strcasecmp(key,"clock24"))       c->clock24       = clampi(atoi(val),0,1);
    else if(!strcasecmp(key,"policy"))        c->policy        = config_policy_from_str(val);
    /* unknown key: ignored */
}

int config_load(const char *path, Config *c){
    sf_recover(path);
    FILE *f=fopen(path,"r");
    if(!f) return -1;
    char line[512];
    while(fgets(line,sizeof line,f)){
        char *s=trim(line);
        if(*s==0 || *s=='#') continue;         /* blank / comment */
        char *eq=strchr(s,'=');
        if(!eq) continue;                       /* malformed: no '=' */
        *eq=0;
        char *key=trim(s), *val=eq+1;
        cut_comment(val);                       /* `value   # note` -> `value` */
        val=trim(val);
        if(*key==0) continue;                   /* empty key */
        apply(c,key,val);
    }
    fclose(f);
    return 0;
}

int config_save(const char *path, const Config *c){
    SafeFile sf;
    FILE *f=sf_open(&sf,path,"w");
    if(!f) return -1;
    fprintf(f,"# CYD Palm device config. The device keeps its passwords in its own flash,\n");
    fprintf(f,"# not here; a password typed into this file is moved there on the next boot.\n");
    fprintf(f,"# `key = value`, one per line. '#' starts a comment.\n\n");
    /* In try order, most recently connected first -- so the file reads the way
     * the device behaves, and an empty slot writes as an empty value rather than
     * vanishing (a missing key would silently keep whatever was loaded before). */
    for(int i = 0; i < CFG_WIFI_N; i++){
        char ks[16], kp[16];
        wifi_keys(i, ks, sizeof ks, kp, sizeof kp);
        fprintf(f,"%s = %s\n", ks, c->wifi[i].ssid);
        fprintf(f,"%s = %s\n", kp, c->wifi[i].pass);
    }
    fprintf(f,"dav_user = %s\n",      c->dav_user);
    fprintf(f,"dav_pass = %s\n",      c->dav_pass);
    fprintf(f,"dav_base = %s\n",      c->dav_base);
    fprintf(f,"dav_card_base = %s\n", c->dav_card_base);
    fprintf(f,"cal_coll = %s\n",      c->cal_coll);
    fprintf(f,"todo_coll = %s\n",     c->todo_coll);
    fprintf(f,"card_coll = %s\n",     c->card_coll);
    fprintf(f,"timezone = %s\n",      c->timezone);
    /* never -1: writing the file is what settles the question for good */
    fprintf(f,"loc_auto = %d\n",      c->loc_auto > 0 ? 1 : 0);
    fprintf(f,"loc_name = %s\n",      c->loc_name);
    fprintf(f,"latitude = %s\n",      c->latitude);
    fprintf(f,"longitude = %s\n",     c->longitude);
    fprintf(f,"owner = %s\n",         c->owner);
    fprintf(f,"world1 = %s\n",        c->world1);
    fprintf(f,"world2 = %s\n",        c->world2);
    fprintf(f,"brightness = %d\n",    c->brightness);
    fprintf(f,"backlight_sec = %d\n", c->backlight_sec);
    fprintf(f,"clock24 = %d\n",       c->clock24);
    fprintf(f,"policy = %s\n",        config_policy_to_str(c->policy));
    return sf_commit(&sf, !ferror(f));
}
