/* config.h -- runtime device configuration (the Preferences app backend).
 *
 * This parses/serialises a plain `key = value` file on the SD card, which is
 * how the device's settings persist (Settings edits them at runtime). Format: one
 * `key = value` per line, surrounding whitespace ignored, unknown keys skipped,
 * malformed lines skipped (robust against a hand-edited file).
 *
 * COMMENTS: a '#' at the start of a line comments the whole line. A '#' that
 * FOLLOWS whitespace ends the value ("timezone = UTC   # note"). A '#' with no
 * space before it is an ordinary character, so a password may contain one.
 *
 * PASSWORDS ARE NOT IN Config. A config.ini may still carry them (a card from
 * before the device kept its own, or one typed on a computer), so the reader
 * and writer take a separate ConfigSecrets, which the caller holds only for as
 * long as it takes to move them into the device's store and wipes afterwards.
 * The resident Config keeps a "has a password" flag per network and for the
 * account, and nothing else (firmware/main/appcfg.h, secretstore.h).
 */
#ifndef CONFIG_H
#define CONFIG_H
#include <stddef.h>
#include <stdint.h>

/* conflict policy values match ConflictPolicy in sync.h (server/local/both). */
enum { CFG_POL_SERVER = 0, CFG_POL_LOCAL = 1, CFG_POL_BOTH = 2 };

/* How many Wi-Fi networks the device remembers. Four, because that covers home /
 * work / phone hotspot / one more, and because four rows fit one non-scrolling
 * list on a 240x184 content area -- a fifth would buy a scrollbar. */
#define CFG_WIFI_N 4

typedef struct {
    char    ssid[64];
    uint8_t has_pass;          /* a password is stored for this SSID. Runtime
                                * only: set by appcfg, never read or written by
                                * the file functions below. It moves with the
                                * slot when config_wifi_promote reorders them. */
} WifiNet;

/* The passwords a config.ini can carry, by slot as the file lists them. */
typedef struct {
    char wifi_pass[CFG_WIFI_N][64];
    char dav_pass[64];
} ConfigSecrets;

typedef struct {
    /* THE ARRAY ORDER IS THE TRY ORDER, and slot 0 is whichever network
     * connected most recently: a successful join promotes its slot to the front
     * (config_wifi_promote). So the common case -- you are where you were last
     * time -- connects on the first attempt, and the file needs no separate
     * "last used" key that could disagree with the list it describes. */
    WifiNet wifi[CFG_WIFI_N];
    char dav_user[128];        /* Apple ID email                         */
    uint8_t dav_has_pass;      /* an app-specific password is stored (runtime only) */
    char dav_base[128];        /* caldav host, e.g. https://caldav.icloud.com   */
    char dav_card_base[128];   /* carddav host, e.g. https://contacts.icloud.com */
    char cal_coll[192];        /* Date Book collection path              */
    char todo_coll[192];       /* To Do (Reminders list) collection      */
    char card_coll[192];       /* Address book collection                */
    char timezone[48];         /* e.g. America/New_York ("" = floating)  */
    /* Weather location. Stored as text exactly as typed so a hand-edited
     * config.ini round-trips unchanged, and because the device has no use for
     * the number beyond pasting it into a URL. Empty = weather is not fetched
     * (see PRODUCT_PLAN: lat/lon in config for v1, IP geolocation later). */
    char latitude[24];
    char longitude[24];
    /* Where the coordinates CAME FROM, which decides whether a sync may improve
     * them. Picking a city off a list of two dozen is not "I am in New York", it
     * is "New York is the nearest one you offered me" -- so a location that was
     * derived (from the zone, from the city list, from a previous IP lookup)
     * stays open to refinement, and only coordinates somebody actually typed are
     * left alone.
     *
     * THREE states, because two were not enough. 1 = approximate, 0 = pinned,
     * and **-1 = the file did not say**, which is what every card written before
     * this field existed looks like. Defaulting that silence to "pinned" was the
     * first attempt and it was wrong in the common case: nearly every such card
     * holds coordinates the CITY LIST put there, so the rule froze exactly the
     * devices the refinement was built for, while protecting a hand-edited
     * minority. Defaulting it to "approximate" would have the opposite fault and
     * a worse one -- silently overwriting numbers somebody typed.
     *
     * So -1 is resolved once, by the only code that can tell the two apart:
     * appcfg_load() asks whether the coordinates are EXACTLY a built-in city's,
     * which is a thing only the pickers write. config_save() always writes 0 or
     * 1, so a card is asked this question at most once. */
    int  loc_auto;
    /* What to call the place, for the Location panel. Coordinates are unreadable
     * and the refined ones will not match any city in the built-in table, so
     * without this the panel goes from "Detroit" to "42.33, -83.05" the moment it
     * gets MORE accurate -- which reads as a regression. Empty = show the
     * numbers, which is the honest display for a value that was typed. */
    char loc_name[32];
    char owner[32];            /* owner's name, shown on the lock screen   */
    char world1[48];           /* lock-screen world clock 1 (IANA zone, "" = off) */
    char world2[48];           /* lock-screen world clock 2 (IANA zone, "" = off) */
    int  brightness;           /* backlight, 0..100                      */
    int  backlight_sec;        /* idle seconds -> dim backlight, 0=never  */
    int  clock24;              /* 0 = 12-hour clock, 1 = 24-hour          */
    int  policy;               /* CFG_POL_*                              */
    /* Date Book syncs a window of days, not the whole account (sync_set_window):
     * from cal_back days before today to cal_ahead days after it. cal_ahead 0
     * syncs the whole calendar. */
    int  cal_back;             /* days before today, 0..366 (default 1)  */
    int  cal_ahead;            /* days after today, 0..730 (default 14)  */
} Config;

/* populate with safe defaults (empty creds, sensible timers). */
void config_defaults(Config *c);

/* load from `path` over the current contents of *c (call config_defaults first,
 * or pre-fill). Each recognised key overrides its field; the rest stay as-is.
 * Password keys go to *sec, or are skipped when sec is NULL.
 * Returns 0 if the file was read (even partially), -1 if it could not be opened. */
int  config_load(const char *path, Config *c, ConfigSecrets *sec);

/* write *c to `path` as a commented key=value file, with the passwords from
 * *sec, or with every password key empty when sec is NULL. Returns 0 or -1. */
int  config_save(const char *path, const Config *c, const ConfigSecrets *sec);

/* zero a buffer that held a password, in a way the compiler cannot drop. */
void config_wipe(void *p, size_t n);

/* Move Wi-Fi slot `i` to the front, keeping the order of the rest. Call it when a
 * network connects, so the next sync tries that one first. Returns 1 if the order
 * actually changed (i.e. the caller should persist), 0 if it was already first or
 * `i` is out of range. */
int  config_wifi_promote(Config *c, int i);

/* map a policy string ("server"/"local"/"both") to CFG_POL_*, default server. */
int  config_policy_from_str(const char *s);
const char *config_policy_to_str(int policy);

#endif
