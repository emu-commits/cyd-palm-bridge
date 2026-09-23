/* appcfg.h -- the device's active runtime configuration.
 *
 * Loads /sdcard/config.ini over config_defaults() (safe hosts/timers), then
 * fills the password fields from the device's own flash (secretstore.h) --
 * the card holds everything EXCEPT the passwords. See bridge/config.[ch] for
 * the parser/serialiser and the Config struct.
 *
 * SENSITIVE: the Config holds the Wi-Fi and app-specific passwords -- never log
 * the password fields.
 */
#ifndef APPCFG_H
#define APPCFG_H
#include "config.h"

/* (Re)load the active config: defaults <- /sdcard/config.ini <- stored passwords.
 * A password found in config.ini is moved into the store and the file rewritten.
 * Safe to call before SD is mounted (config.ini just won't be found). */
void appcfg_load(void);

/* the active runtime config (loads it on first use). */
const Config* appcfg(void);

/* mutable handle for the Preferences editor; follow edits with appcfg_save(). */
Config* appcfg_mut(void);

/* did /sdcard/config.ini actually exist (vs. running on the defaults)? */
int appcfg_from_sd(void);

/* write the active config to /sdcard/config.ini -- WITHOUT the passwords, which
 * go to the store (if the store refuses, the file keeps them). 0 on success. */
int appcfg_save(void);

#endif
