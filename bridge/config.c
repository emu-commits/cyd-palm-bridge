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

/* trim leading/trailing ASCII whitespace in place; returns the start pointer. */
static char *trim(char *s){
    while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n') s++;
    char *e=s+strlen(s);
    while(e>s && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e=0;
    return s;
}

/* clamp an int to [lo,hi]. */
static int clampi(int v,int lo,int hi){ return v<lo?lo:v>hi?hi:v; }

static void apply(Config *c, const char *key, const char *val){
    if(!strcasecmp(key,"wifi_ssid"))      setstr(c->wifi_ssid,     sizeof c->wifi_ssid, val);
    else if(!strcasecmp(key,"wifi_pass")) setstr(c->wifi_pass,     sizeof c->wifi_pass, val);
    else if(!strcasecmp(key,"dav_user"))  setstr(c->dav_user,      sizeof c->dav_user, val);
    else if(!strcasecmp(key,"dav_pass"))  setstr(c->dav_pass,      sizeof c->dav_pass, val);
    else if(!strcasecmp(key,"dav_base"))  setstr(c->dav_base,      sizeof c->dav_base, val);
    else if(!strcasecmp(key,"dav_card_base")) setstr(c->dav_card_base, sizeof c->dav_card_base, val);
    else if(!strcasecmp(key,"cal_coll"))  setstr(c->cal_coll,      sizeof c->cal_coll, val);
    else if(!strcasecmp(key,"todo_coll")) setstr(c->todo_coll,     sizeof c->todo_coll, val);
    else if(!strcasecmp(key,"card_coll")) setstr(c->card_coll,     sizeof c->card_coll, val);
    else if(!strcasecmp(key,"timezone"))  setstr(c->timezone,      sizeof c->timezone, val);
    else if(!strcasecmp(key,"world1"))    setstr(c->world1,        sizeof c->world1, val);
    else if(!strcasecmp(key,"world2"))    setstr(c->world2,        sizeof c->world2, val);
    else if(!strcasecmp(key,"brightness"))    c->brightness    = clampi(atoi(val),0,100);
    else if(!strcasecmp(key,"backlight_sec")) c->backlight_sec = clampi(atoi(val),0,3600);
    else if(!strcasecmp(key,"clock24"))       c->clock24       = clampi(atoi(val),0,1);
    else if(!strcasecmp(key,"policy"))        c->policy        = config_policy_from_str(val);
    /* unknown key: ignored */
}

int config_load(const char *path, Config *c){
    FILE *f=fopen(path,"r");
    if(!f) return -1;
    char line[512];
    while(fgets(line,sizeof line,f)){
        char *s=trim(line);
        if(*s==0 || *s=='#') continue;         /* blank / comment */
        char *eq=strchr(s,'=');
        if(!eq) continue;                       /* malformed: no '=' */
        *eq=0;
        char *key=trim(s), *val=trim(eq+1);
        if(*key==0) continue;                   /* empty key */
        apply(c,key,val);
    }
    fclose(f);
    return 0;
}

int config_save(const char *path, const Config *c){
    FILE *f=fopen(path,"w");
    if(!f) return -1;
    fprintf(f,"# CYD Palm device config. Holds Wi-Fi + iCloud passwords -- keep private.\n");
    fprintf(f,"# `key = value`, one per line. '#' starts a comment.\n\n");
    fprintf(f,"wifi_ssid = %s\n",     c->wifi_ssid);
    fprintf(f,"wifi_pass = %s\n",     c->wifi_pass);
    fprintf(f,"dav_user = %s\n",      c->dav_user);
    fprintf(f,"dav_pass = %s\n",      c->dav_pass);
    fprintf(f,"dav_base = %s\n",      c->dav_base);
    fprintf(f,"dav_card_base = %s\n", c->dav_card_base);
    fprintf(f,"cal_coll = %s\n",      c->cal_coll);
    fprintf(f,"todo_coll = %s\n",     c->todo_coll);
    fprintf(f,"card_coll = %s\n",     c->card_coll);
    fprintf(f,"timezone = %s\n",      c->timezone);
    fprintf(f,"world1 = %s\n",        c->world1);
    fprintf(f,"world2 = %s\n",        c->world2);
    fprintf(f,"brightness = %d\n",    c->brightness);
    fprintf(f,"backlight_sec = %d\n", c->backlight_sec);
    fprintf(f,"clock24 = %d\n",       c->clock24);
    fprintf(f,"policy = %s\n",        config_policy_to_str(c->policy));
    fclose(f);
    return 0;
}
