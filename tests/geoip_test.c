/* geoip_test.c -- the IP-location reply parser. No network.
 *
 * The fixtures are the shapes ip-api.com's CSV endpoint actually returns, kept
 * verbatim, for the same reason wx_test keeps real Open-Meteo CSV: a parser
 * tested only against input the author imagined passes right up until the
 * service answers the way it always did. */
#include <stdio.h>
#include <string.h>
#include "../bridge/geoip.h"

static int failures;
static void CK(int c, const char *m){
    if(!c){ failures++; printf("  FAIL: %s\n", m); } else printf("  ok: %s\n", m);
}

int main(void){
    printf("== geoip ==\n");
    char lat[24], lon[24], tz[48];

    /* the ordinary answer */
    CK(geoip_parse("success,42.3601,-71.0589,America/New_York",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 1, "a good reply parses");
    CK(!strcmp(lat, "42.3601"), "latitude copied through as text");
    CK(!strcmp(lon, "-71.0589"), "longitude keeps its sign");
    CK(!strcmp(tz, "America/New_York"), "timezone comes along too");

    /* text, not a float: config.ini stores these as typed and a value that
     * round-trips unchanged is one a human can compare against a map */
    CK(geoip_parse("success,51.5074,-0.1278,Europe/London",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 1, "London parses");
    CK(!strcmp(lon, "-0.1278"), "a leading zero and sign survive verbatim");

    /* a trailing newline is what a fetched body actually looks like */
    CK(geoip_parse("success,35.6895,139.6917,Asia/Tokyo\n",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 1, "trailing newline ok");
    CK(!strcmp(tz, "Asia/Tokyo"), "...and does not end up inside the zone");

    /* the service refusing is a NORMAL answer -- a device behind a carrier NAT
     * gets exactly this -- and it must write nothing at all */
    strcpy(lat, "SENTINEL"); strcpy(lon, "SENTINEL"); strcpy(tz, "SENTINEL");
    CK(geoip_parse("fail,private range", lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 0,
       "a refusal is not a location");
    CK(lat[0] == 0 && lon[0] == 0 && tz[0] == 0, "...and nothing is left in the outputs");

    /* half a reply must not half-configure the device: empty is how "not set" is
     * spelled, and one coordinate set with the other empty would look deliberate */
    CK(geoip_parse("success,42.3601", lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 0,
       "a truncated reply is rejected");
    CK(lat[0] == 0 && lon[0] == 0, "...leaving neither coordinate behind");

    /* junk where a number belongs, which is how a captive portal's HTML lands */
    CK(geoip_parse("success,N/A,N/A,", lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 0,
       "non-numeric coordinates rejected");
    CK(geoip_parse("<html><head><title>Redirect", lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 0,
       "an HTML body is rejected");
    CK(geoip_parse("", lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 0, "empty body rejected");

    /* a usable coordinate with an unusable zone is still a win */
    CK(geoip_parse("success,-33.8688,151.2093,", lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 1,
       "a missing zone does not sink the coordinates");
    CK(!strcmp(lat, "-33.8688") && tz[0] == 0, "...the zone is simply left unset");
    CK(geoip_parse("success,-33.8688,151.2093,not a zone",
                   lat, sizeof lat, lon, sizeof lon, tz, sizeof tz) == 1, "a spaced zone is ignored");
    CK(tz[0] == 0, "...rather than written into config.ini");

    /* NULL outputs are allowed: a caller that only wants the coordinates */
    CK(geoip_parse("success,1.3521,103.8198,Asia/Singapore",
                   lat, sizeof lat, lon, sizeof lon, NULL, 0) == 1, "a NULL output is fine");

    /* the query string and the parser are one decision: the reply is positional,
     * so a field added or reordered in the URL silently shifts every column */
    const char *u = geoip_url();
    CK(strstr(u, "fields=status,lat,lon,timezone") != NULL,
       "the URL asks for exactly the fields this parser reads, in order");
    CK(strncmp(u, "http://", 7) == 0, "plain HTTP, deliberately (see geoip.c)");

    printf(failures ? "\nGeoIP gate: %d FAIL\n" : "\nGeoIP gate: OK\n", failures);
    return failures ? 1 : 0;
}
