/* geoip.h -- work out roughly where the device is from its public IP.
 *
 * THE POINT: nobody should have to type a coordinate. Latitude and longitude
 * were the last two values in Settings that could only be entered as numbers,
 * and a wrong one fails silently -- weather simply never appears. A device that
 * is on the internet long enough to fetch a forecast already knows enough to
 * locate itself to the nearest town, which is all a forecast is good for.
 *
 * WHY NOT WI-FI POSITIONING: settled in PRODUCT_PLAN.md (2026-08-19) and not
 * reopened by the Wi-Fi scan that W5 added. A forecast's resolution is
 * kilometres, so locating the device to tens of metres is precision that gets
 * discarded on arrival; Mozilla's free service retired in 2024, and Google's is
 * billable with a key that would have to ship inside the device, where it leaks.
 *
 * WHY CSV: the same reason wxfetch asks Open-Meteo for `&format=csv`. This
 * device has no JSON parser and no heap to spare for one. ip-api.com will answer
 * in CSV, so the reply is one short line and the parser is this file.
 *
 * WHAT IT COSTS: one plain HTTP GET, made ONLY when the location is not already
 * set -- so once in a device's life, not once per sync. Nothing is sent but the
 * request; the service reads the source address of the packet, which is the same
 * address every server the device talks to already sees. The answer is the
 * PUBLIC IP's city, so a VPN or a mobile hotspot backhauled elsewhere will place
 * the device wrong -- which is why Settings keeps a city list and two editable
 * numbers as the correction path, and why this never overwrites a value that is
 * already there.
 */
#ifndef GEOIP_H
#define GEOIP_H

/* The URL to fetch. Fixed, no key, no account. Kept here so the host test and
 * the device agree about exactly which fields are asked for and in what order --
 * the reply is positional, so the query string and the parser are one decision. */
const char *geoip_url(void);

/* Parse the reply. `csv` is the body as fetched (one line; a trailing newline is
 * fine). On success writes the coordinates as TEXT -- copied through rather than
 * rounded through a float, because config.ini stores them as text and a value
 * that round-trips unchanged is one a human can compare against a map.
 *
 * `city` receives the place name, which is not decoration: a refined coordinate
 * matches no city in the built-in table, so without a name the Location panel
 * would go from "Detroit" to "42.33, -83.05" at the exact moment it became MORE
 * accurate, and read as a regression.
 *
 * `tz` receives the IANA zone the service reports, which is a bonus worth having:
 * a device that has never been configured has no zone either, and this is the one
 * moment it can learn both. Any of the outputs may be NULL.
 *
 * Returns 1 only if the service said "success" AND both coordinates are
 * plausible numbers. A failure reply ("fail,private range") returns 0 and writes
 * nothing -- an unparsed field must never reach config.ini as an empty string,
 * because empty is how "not set" is spelled and it would look deliberate.
 */
int geoip_parse(const char *csv,
                char *lat, int latcap,
                char *lon, int loncap,
                char *tz,  int tzcap,
                char *city, int citycap);

#endif
