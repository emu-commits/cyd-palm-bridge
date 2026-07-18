/* config.h -- runtime device configuration (the Preferences app backend).
 *
 * Today Wi-Fi + iCloud + per-app collections are compile-time (secrets.h), so
 * changing them means a reflash. This parses/serialises a plain `key = value`
 * file on the SD card so Preferences can edit them at runtime. Format: one
 * `key = value` per line, `#` comments, surrounding whitespace ignored, unknown
 * keys skipped, malformed lines skipped (robust against a hand-edited file).
 *
 * NOTE: this file holds the Wi-Fi and app-specific passwords, exactly like
 * secrets.h did -- treat it as sensitive; never log the password fields.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* conflict policy values match ConflictPolicy in sync.h (server/local/both). */
enum { CFG_POL_SERVER = 0, CFG_POL_LOCAL = 1, CFG_POL_BOTH = 2 };

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char dav_user[128];        /* Apple ID email                         */
    char dav_pass[64];         /* app-specific password (with dashes)    */
    char dav_base[128];        /* caldav host, e.g. https://caldav.icloud.com   */
    char dav_card_base[128];   /* carddav host, e.g. https://contacts.icloud.com */
    char cal_coll[192];        /* Date Book collection path              */
    char todo_coll[192];       /* To Do (Reminders list) collection      */
    char card_coll[192];       /* Address book collection                */
    char timezone[48];         /* e.g. America/New_York ("" = floating)  */
    char news_feed[3][160];    /* RSS/Atom feed URLs for the News reader ("" = unused) */
    int  brightness;           /* backlight, 0..100                      */
    int  backlight_sec;        /* idle seconds -> dim backlight, 0=never  */
    int  policy;               /* CFG_POL_*                              */
} Config;

/* populate with safe defaults (empty creds, sensible timers). */
void config_defaults(Config *c);

/* load from `path` over the current contents of *c (call config_defaults first,
 * or pre-fill). Each recognised key overrides its field; the rest stay as-is.
 * Returns 0 if the file was read (even partially), -1 if it could not be opened. */
int  config_load(const char *path, Config *c);

/* write *c to `path` as a commented key=value file. Returns 0 or -1. */
int  config_save(const char *path, const Config *c);

/* map a policy string ("server"/"local"/"both") to CFG_POL_*, default server. */
int  config_policy_from_str(const char *s);
const char *config_policy_to_str(int policy);

#endif
