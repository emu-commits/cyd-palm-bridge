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

/* boot: load the persisted epoch from NVS into the system clock (no-op if none
 * saved yet, e.g. first boot before the first sync). Call early, before the UI. */
void clock_restore(void);

/* persist the current time to NVS. Ignores an unset clock (< 2024) so a pre-sync
 * 1970 time never overwrites a good checkpoint. Call after SNTP + periodically. */
void clock_checkpoint(void);

/* start a periodic esp_timer that checkpoints, so an abrupt power-off loses at
 * most one interval of wall-clock accuracy. */
void clock_start_autosave(void);

/* set the system timezone so localtime() shows the user's wall clock. Accepts a
 * POSIX TZ string directly, or maps a few common IANA names; unknown -> UTC. */
void clock_set_tz(const char *tz);

/* enumerate the built-in DST-aware timezone list (for the picker UI). */
int clock_zone_count(void);
const char *clock_zone_name(int i);   /* IANA name, e.g. "America/New_York" */

/* describe the CURRENT wall clock under the active TZ, e.g. "EDT -0400 (DST)".
 * Reflects the system time + whichever TZ clock_set_tz() last applied. */
void clock_now_desc(char *out, int cap);

#endif
