/* clock.h -- durable wall-clock for a no-RTC device.
 *
 * The base CYD has no battery-backed RTC, so the system time is lost on every
 * power-off. This module persists the epoch to NVS and restores it on boot, so
 * the date/time survive a power cycle (the intended usage: power on/off between
 * uses). The restored clock is as-of the last checkpoint; a Wi-Fi HotSync runs
 * SNTP and corrects it exactly (then re-checkpoints). Accuracy across a long
 * power-off is bounded by the checkpoint interval + the off duration -- good
 * enough for a PDA, and every sync re-anchors it to real time.
 */
#ifndef CLOCK_H
#define CLOCK_H
#include <time.h>   /* time_t for clock_zone_hhmm() */

/* boot: load the persisted epoch from NVS into the system clock (no-op if none
 * saved yet, e.g. first boot before the first sync). Call early, before the UI. */
void clock_restore(void);

/* persist the current time to NVS. Ignores an unset clock (< 2024) so a pre-sync
 * 1970 time never overwrites a good checkpoint. Call after SNTP + periodically. */
void clock_checkpoint(void);

/* start a periodic esp_timer that checkpoints, so an abrupt power-off loses at
 * most one interval of wall-clock accuracy. */
void clock_start_autosave(void);

/* ---- drift meter --------------------------------------------------------
 * How far the free-running clock wanders between syncs. This is the number that
 * decides whether the device needs a real RTC part at all (BACKLOG.md, "A real
 * RTC part"): the estimates there span seconds/day to minutes/day depending on
 * how much of the time is spent in light sleep on the uncalibrated RC, and
 * guessing has already shaped one hardware decision too many.
 *
 * Bracket the SNTP call with these two. begin() takes the clock as it stands
 * plus a MONOTONIC mark; end() reports what SNTP moved it by, over how long, and
 * the implied rate, then re-anchors for the next sync. The monotonic mark is
 * what makes it honest: the seconds spent waiting for SNTP to answer are real
 * elapsed time, not clock error, and have to come off the correction.
 *
 * A sample whose interval contains a power loss is reported but NOT counted --
 * the correction then measures the outage, not the drift. Each sample is logged
 * and appended to /sdcard/drift.log, because the experiment runs on battery with
 * no USB attached and a serial-only reading would never be read. */
void clock_sync_begin(void);
void clock_sync_end(int synced);

/* set the system timezone so localtime() shows the user's wall clock. Accepts a
 * POSIX TZ string directly, or maps a few common IANA names; unknown -> UTC. */
void clock_set_tz(const char *tz);

/* enumerate the built-in DST-aware timezone list (for the picker UI). */
int clock_zone_count(void);
const char *clock_zone_name(int i);   /* IANA name, e.g. "America/New_York" */

/* describe the CURRENT wall clock under the active TZ, e.g. "EDT -0400 (DST)".
 * Reflects the system time + whichever TZ clock_set_tz() last applied. */
void clock_now_desc(char *out, int cap);

/* format the wall clock of an arbitrary zone at time t as "HH:MM" (24h), DST-aware.
 * `iana` is an IANA name (or a POSIX TZ string) from the built-in table; unknown ->
 * UTC. Restores the active TZ afterwards, so the world-clock tiles don't disturb the
 * system zone. `out` is left empty on bad args. */
void clock_zone_hhmm(const char *iana, time_t t, char *out, int cap);

#endif
