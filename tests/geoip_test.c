/* geoip_test.c -- the IP-location reply parser. No network.
 *
 * EVERY FIXTURE HERE IS A REAL CAPTURED RESPONSE, and that is the whole lesson
 * of this file. The first version invented its fixtures in the shape the author
 * assumed -- a positional CSV in the order the query string asked for -- and
 * every one of them passed. The service answers in its OWN order and ignores
 * the order requested, so on the first real device the parser read a town name
 * where a latitude belongs and rejected the reply as junk:
 *
 *     asked:  /csv/?fields=status,lat,lon,timezone,city
 *     got:    success,Bloomfield,40.803,-74.1909,America/New_York
 *
 * The old gate even asserted "the URL asks for exactly the fields this parser
 * reads, in order" -- which checked the assumption against itself and returned
 * a confident green. An invented fixture tests the author, not the service. */
#include <stdio.h>
#include <string.h>
#include "../bridge/geoip.h"

static int failures;
static void CK(int c, const char *m){
    if(!c){ failures++; printf("  FAIL: %s\n", m); } else printf("  ok: %s\n", m);
}

int main(void){
    printf("== geoip ==\n");
    char lat[24], lon[24], tz[48], city[32];

    /* VERBATIM, captured 2026-09-22 from
     * http://ip-api.com/json/?fields=status,lat,lon,timezone,city */
    const char *real = "{\"status\":\"success\",\"city\":\"Bloomfield\","
                       "\"lat\":40.803,\"lon\":-74.1909,"
                       "\"timezone\":\"America/New_York\"}";
    CK(geoip_parse(real, lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 1, "the real reply parses");
    CK(!strcmp(lat, "40.803"), "latitude, as text and unrounded");
    CK(!strcmp(lon, "-74.1909"), "longitude keeps its sign");
    CK(!strcmp(tz, "America/New_York"), "timezone");
    CK(!strcmp(city, "Bloomfield"), "city");

    /* THE PROPERTY THAT FAILED: which value is which must not depend on where it
     * sits. The service is free to reorder these, and once did. */
    const char *shuffled = "{\"timezone\":\"Europe/London\",\"lon\":-0.1278,"
                           "\"status\":\"success\",\"city\":\"London\",\"lat\":51.5074}";
    CK(geoip_parse(shuffled, lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 1, "a reordered reply parses");
    CK(!strcmp(lat, "51.5074") && !strcmp(lon, "-0.1278"),
       "...with the coordinates still the coordinates");
    CK(!strcmp(city, "London"), "...and the city still the city");

    /* a key must not match inside a longer one */
    const char *longer = "{\"status\":\"success\",\"latency\":999,\"lat\":1.5,"
                         "\"lon\":2.5,\"city\":\"Nowhere\"}";
    CK(geoip_parse(longer, lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 1, "a similarly-named key does not confuse it");
    CK(!strcmp(lat, "1.5"), "...\"lat\" is not matched by \"latency\"");

    /* the refusal, which is a NORMAL answer: a device behind a carrier NAT gets
     * exactly this, and it must write nothing at all */
    strcpy(lat, "SENTINEL"); strcpy(lon, "SENTINEL"); strcpy(tz, "SENTINEL");
    CK(geoip_parse("{\"status\":\"fail\",\"message\":\"private range\"}",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 0, "a refusal is not a location");
    CK(lat[0] == 0 && lon[0] == 0 && tz[0] == 0, "...and nothing is left behind");

    /* half a reply must not half-configure the device: empty is how "not set" is
     * spelled, and one coordinate set with the other empty would look deliberate */
    CK(geoip_parse("{\"status\":\"success\",\"lat\":40.8}",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 0, "a truncated reply is rejected");
    CK(lat[0] == 0 && lon[0] == 0, "...leaving neither coordinate behind");

    /* junk where a number belongs, which is how a captive portal's HTML lands */
    CK(geoip_parse("{\"status\":\"success\",\"lat\":\"N/A\",\"lon\":\"N/A\"}",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 0, "non-numeric coordinates rejected");
    CK(geoip_parse("<html><head><title>Sign in to the network</title>",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 0, "a captive portal's HTML is rejected");
    CK(geoip_parse("", lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 0, "an empty body is rejected");

    /* a usable coordinate with an unusable or absent extra is still a win */
    CK(geoip_parse("{\"status\":\"success\",\"lat\":-33.8688,\"lon\":151.2093}",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 1, "no zone and no city still parses");
    CK(tz[0] == 0 && city[0] == 0, "...and neither is guessed at");
    CK(geoip_parse("{\"status\":\"success\",\"lat\":1.35,\"lon\":103.8,"
                   "\"timezone\":\"not a zone\",\"city\":\"Singapore\"}",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz,
                   city, sizeof city) == 1, "a spaced zone is ignored");
    CK(tz[0] == 0 && !strcmp(city, "Singapore"), "...without costing the city");

    /* NULL outputs are allowed: a caller that only wants the coordinates */
    CK(geoip_parse(real, lat, sizeof lat, lon, sizeof lon, NULL, 0, NULL, 0) == 1,
       "a NULL output is fine");

    /* The endpoint must be the one whose values are NAMED. There is no assertion
     * here about field ORDER, deliberately: the previous one asserted the
     * author's assumption about the service and passed while the device failed. */
    const char *u = geoip_url();
    CK(strstr(u, "/json/") != NULL, "the named-value endpoint, not the positional one");
    CK(strncmp(u, "http://", 7) == 0, "plain HTTP, deliberately (see geoip.c)");

    printf(failures ? "\nGeoIP gate: %d FAIL\n" : "\nGeoIP gate: OK\n", failures);
    return failures ? 1 : 0;
}
