/* wxfetch.h -- turn an Open-Meteo CSV forecast into the dashboard's WxCache.
 *
 * The device has no JSON parser and no heap to spare for one, so the fetch asks
 * Open-Meteo for `&format=csv` instead: the same data in ~1.5 KB of lines we can
 * read one at a time. This file is the parsing half -- portable, host-gated
 * (tests/wx_test.c), no network -- so the device-only part stays a plain GET to
 * a file, exactly like an RSS feed.
 *
 * The CSV has three or four blocks separated by blank lines, each with its own
 * header row:
 *   latitude,longitude,elevation,utc_offset_seconds,timezone,timezone_abbreviation
 *   time,temperature_2m (F),weather_code (wmo code)        <- "current"
 *   time,temperature_2m (F),precipitation_probability (%)  <- "hourly"
 *   time,sunrise (iso8601),sunset (iso8601)                <- "daily"
 * Blocks are identified by their header, never by position, so an added or
 * reordered field cannot silently shift a column.
 */
#ifndef WXFETCH_H
#define WXFETCH_H
#include <stdint.h>
#include "dash.h"

/* Build the URL that yields exactly the fields wx_parse_file expects. `lat` and
 * `lon` are the config strings, copied in as typed. Returns the length written,
 * or 0 if either is empty or not a plausible number. */
int wx_build_url(char *out, int cap, const char *lat, const char *lon);

/* Same, for the separate air-quality endpoint (US AQI). */
int wx_build_aqi_url(char *out, int cap, const char *lat, const char *lon);

/* Parse an Open-Meteo CSV forecast into *out. `now` is the current Unix time,
 * used to pick which hourly rows are still ahead (the feed starts at midnight
 * local, so roughly half of them are in the past) and to stamp gen_epoch.
 * Returns 1 if at least the current conditions were found, 0 otherwise.
 * Never leaves *out half-filled: it writes into a local and copies on success. */
int wx_parse_file(const char *path, int64_t now, WxCache *out);

/* Read the AQI CSV and return the US AQI, or -1 if absent/unparsable. */
int wx_parse_aqi_file(const char *path);

#endif
