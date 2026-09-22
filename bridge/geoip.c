/* geoip.c -- see geoip.h. One short body in, four strings out. No heap. */
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
 * device that handshake is the single largest allocation a sync makes.
 *
 * THE JSON ENDPOINT, NOT THE CSV ONE, AND THAT IS NOT A PREFERENCE. The CSV
 * endpoint answers in the service's OWN field order and ignores the order asked
 * for: request `status,lat,lon,timezone,city` -- or `city,status,timezone,lon,
 * lat`, which was tried -- and both come back as
 *
 *     success,Bloomfield,40.803,-74.1909,America/New_York
 *
 * so a positional parser reads a town name where a latitude belongs and throws
 * the whole reply away. `fields` still trims the response to the five values
 * that are wanted; it just cannot dictate their order, and only the JSON form
 * says which value is which. */
#define GEOIP_URL "http://ip-api.com/json/?fields=status,lat,lon,timezone,city"

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

/* Look up ONE named value in a flat JSON object.
 *
 * THIS IS NOT A JSON PARSER and must not grow into one -- the reason this device
 * asks Open-Meteo for CSV stands. It is a bounded search for `"key":` followed
 * by a copy of the value that follows, which is all a five-key flat object needs
 * and is why the reply can be read without a heap, a tokenizer or a recursion.
 * A string value arrives quoted and a number bare; both end at the first comma
 * or closing brace outside the quotes.
 *
 * Matching includes the colon so that a key cannot match inside a longer one.
 * Returns 1 if the key was found. */
static int json_str(const char *body, const char *key, char *out, int cap){
    if(!body || !key || !out || cap <= 0) return 0;
    out[0] = 0;

    char pat[32];
    int pn = snprintf(pat, sizeof pat, "\"%s\":", key);
    if(pn <= 0 || pn >= (int)sizeof pat) return 0;
    const char *p = strstr(body, pat);
    if(!p) return 0;
    p += pn;
    while(*p == ' ' || *p == '\t') p++;

    int j = 0;
    if(*p == '"'){                      /* a quoted string: copy to the close */
        p++;
        while(*p && *p != '"'){
            if(j < cap - 1) out[j] = *p;
            j++; p++;
        }
    } else {                            /* a bare number or literal */
        while(*p && *p != ',' && *p != '}' && *p != '\n' && *p != '\r'){
            if(j < cap - 1) out[j] = *p;
            j++; p++;
        }
        while(j > 0 && (out[j-1] == ' ' || out[j-1] == '\t')) j--;
    }
    out[j < cap - 1 ? j : cap - 1] = 0;
    return 1;
}

int geoip_parse(const char *body,
                char *lat, int latcap,
                char *lon, int loncap,
                char *tz,  int tzcap,
                char *city, int citycap){
    if(lat && latcap > 0) lat[0] = 0;
    if(lon && loncap > 0) lon[0] = 0;
    if(tz  && tzcap  > 0) tz[0]  = 0;
    if(city && citycap > 0) city[0] = 0;
    if(!body || !body[0]) return 0;

    /* The service's own verdict, first. A refusal is a NORMAL answer here -- a
     * device behind a carrier NAT gets {"status":"fail","message":"private
     * range"} -- so it is checked before anything else is even looked at. */
    char status[16];
    if(!json_str(body, "status", status, sizeof status)) return 0;
    if(strcmp(status, "success")) return 0;

    char la[32], lo[32];
    if(!json_str(body, "lat", la, sizeof la)) return 0;
    if(!json_str(body, "lon", lo, sizeof lo)) return 0;
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
        if(json_str(body, "timezone", z, sizeof z) && z[0] && !strchr(z, ' '))
            snprintf(tz, tzcap, "%s", z);
    }
    if(city && citycap > 0){
        char t[64];
        if(json_str(body, "city", t, sizeof t) && t[0]) snprintf(city, citycap, "%s", t);
    }
    return 1;
}
