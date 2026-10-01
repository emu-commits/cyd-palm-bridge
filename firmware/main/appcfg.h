/* appcfg.h -- the device's active runtime configuration.
 *
 * Loads /sdcard/config.ini over config_defaults() (safe hosts/timers). The
 * card holds everything EXCEPT the passwords, which live in the device's own
 * flash (secretstore.h).
 *
 * PASSWORDS ARE NOT RESIDENT. The Config returned here holds a has-password
 * flag per network and for the account, never a password. Code that needs one
 * (a Wi-Fi join, a sync, discovery) reads it with the getters below into a
 * buffer of its own, uses it, and wipes that buffer with config_wipe() straight
 * away. Never log a password, or anything derived from one. */
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

/* write the active config to /sdcard/config.ini, WITHOUT passwords (except any
 * the store refused to take at load: those stay in the file, since a password
 * on the card is a privacy problem and a password nowhere is a device that
 * cannot connect). 0 on success. */
int appcfg_save(void);

/* ---- passwords: straight to and from the store; nothing is kept here ----
 * The getters fill `out` and return 1 if there is a password (the caller wipes
 * it); 0 and "" if not. The setters store `pass` (NULL or "" removes it), keep
 * the has-password flag in step, and return 0, or -1 if the store refused. */
int  appcfg_wifi_pass(int slot, char *out, size_t cap);
int  appcfg_set_wifi_pass(int slot, const char *pass);
int  appcfg_dav_pass(char *out, size_t cap);
int  appcfg_set_dav_pass(const char *pass);

/* Point Wi-Fi slot `slot` at another network ("" empties it). Passwords are
 * stored by SSID, so this also keeps the slot's has-password flag true to the
 * new name, and removes the old network's password once no slot uses it. Does
 * not save the file: follow with appcfg_save(). */
void appcfg_set_wifi_ssid(int slot, const char *ssid);

#endif
