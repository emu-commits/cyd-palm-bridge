/* appcfg.h -- the device's active runtime configuration.
 *
 * Loads /sdcard/config.ini over the compile-time secrets.h values so Wi-Fi,
 * iCloud login, and per-app collections can be set on the device without a
 * reflash. Precedence: config_defaults() (safe hosts/timers) < secrets.h seed
 * (so a device with no config.ini behaves exactly as before) < config.ini
 * (overrides any field present in the file). See bridge/config.[ch] for the
 * parser/serialiser and the Config struct.
 *
 * SENSITIVE: the Config holds the Wi-Fi and app-specific passwords -- never log
 * the password fields.
 */
#ifndef APPCFG_H
#define APPCFG_H
#include "config.h"

/* (Re)load the active config: defaults <- secrets.h seed <- /sdcard/config.ini.
 * Safe to call before SD is mounted (config.ini just won't be found -> seeds). */
void appcfg_load(void);

/* the active runtime config (loads it on first use). */
const Config* appcfg(void);

/* mutable handle for the Preferences editor; follow edits with appcfg_save(). */
Config* appcfg_mut(void);

/* did /sdcard/config.ini actually exist (vs. falling back to secrets.h)? */
int appcfg_from_sd(void);

/* write the active config to /sdcard/config.ini. Returns 0 on success, -1 else. */
int appcfg_save(void);

#endif
