/* geoip.c -- see geoip.h. One line in, three strings out. No heap, no JSON. */
#include "geoip.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* Plain HTTP, and deliberately so. The free tier of this service does not serve
 * TLS, and there is nothing here worth protecting: the request carries no
 * identity beyond the source address the network already exposes, and the reply
 * is a coordinate the user is about to see on screen and can correct. A
 * man-in-the-middle's worst outcome is the wrong town's weather. Paying the TLS
 * handshake's ~30 KB of heap for that would cost more than it buys -- on this
 * device that handshake is the single largest allocation a sync makes. */
#define GEOIP_URL "http://ip-api.com/csv/?fields=status,lat,lon,timezone"

const char *geoip_url(void){ return GEOIP_URL; }

/* The same coordinate test wxfetch uses before pasting one into a URL: optional
 * sign, digits, at most one dot. Deliberately a copy of the rule rather than a
 * shared function -- this one is a validator for INCOMING data from a third
 * party and that one guards an outgoing URL, and if either ever needs loosening
 * it must not silently loosen the other. */
static int coord_ok(const char *s){
    if(!s || !s[0]) return 0;
    int digits = 0, dots = 0, i = 0;
    if(s[0]=='+' || s[0]=='-') i = 1;
    for(; s[i]; i++){
        if(s[i]=='.'){ if(++dots > 1) return 0; continue; }
        if(!isdigit((unsigned char)s[i])) return 0;
        digits++;
    }
    return digits > 0;
}

/* Copy field `n` (0-based, comma-separated) out of `csv` into `out`, trimming
 * spaces and stopping at the end of the line. Returns 1 if the field existed. */
static int field(const char *csv, int n, char *out, int cap){
    if(!csv || !out || cap <= 0) return 0;
    out[0] = 0;
    const char *p = csv;
    for(int i = 0; i < n; i++){
        p = strchr(p, ',');
        if(!p) return 0;
        p++;
    }
    while(*p == ' ' || *p == '\t') p++;
    int j = 0;
    while(p[j] && p[j] != ',' && p[j] != '\n' && p[j] != '\r'){
        if(j < cap - 1) out[j] = p[j];
        j++;
    }
    int end = j < cap - 1 ? j : cap - 1;
    while(end > 0 && (out[end-1] == ' ' || out[end-1] == '\t')) end--;
    out[end] = 0;
    return 1;
}

int geoip_parse(const char *csv,
                char *lat, int latcap,
                char *lon, int loncap,
                char *tz,  int tzcap){
    if(lat && latcap > 0) lat[0] = 0;
    if(lon && loncap > 0) lon[0] = 0;
    if(tz  && tzcap  > 0) tz[0]  = 0;
    if(!csv || !csv[0]) return 0;

    /* Field 0 is the service's own verdict. A refusal is a normal answer here --
     * a device on a private range behind a carrier NAT gets "fail,private range"
     * -- so it is checked first and nothing else is even looked at. */
    char status[16];
    if(!field(csv, 0, status, sizeof status)) return 0;
    if(strcmp(status, "success")) return 0;

    char la[32], lo[32];
    if(!field(csv, 1, la, sizeof la)) return 0;
    if(!field(csv, 2, lo, sizeof lo)) return 0;
    if(!coord_ok(la) || !coord_ok(lo)) return 0;

    /* Write the outputs only once EVERYTHING has checked out, so a half-parsed
     * reply cannot leave one coordinate set and the other empty -- which would
     * read downstream as a deliberate half-configuration. */
    if(lat && latcap > 0) snprintf(lat, latcap, "%s", la);
    if(lon && loncap > 0) snprintf(lon, loncap, "%s", lo);
    if(tz && tzcap > 0){
        char z[64];
        /* The zone is optional: a good coordinate with a missing or unusable
         * zone is still a win, and the clock has its own way to be set. */
        if(field(csv, 3, z, sizeof z) && z[0] && !strchr(z, ' '))
            snprintf(tz, tzcap, "%s", z);
    }
    return 1;
}
