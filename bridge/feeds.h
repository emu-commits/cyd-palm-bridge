/* feeds.h -- on-SD list of RSS/Atom sources for the News reader.
 *
 * A plain, hand-editable text file: one feed per line, "<on|off>\t name \t url".
 * Loaded once into a small fixed table (no heap, ~4 KB BSS); the Preferences feeds
 * manager edits it and HotSync fetches only the enabled ones. Portable and
 * host-gated (tests/feeds_test.c); the SD path is the only device-specific bit.
 */
#ifndef FEEDS_H
#define FEEDS_H

#define FEEDS_MAX      16
#define FEED_URL_CAP   192
#define FEED_NAME_CAP  48

typedef struct {
    char url[FEED_URL_CAP];
    char name[FEED_NAME_CAP];   /* short label; auto-derived from the host if empty */
    int  enabled;
} Feed;

/* ---- the active in-RAM list ---- */
int         feeds_count(void);
const Feed *feeds_get(int i);            /* NULL if out of range */
int         feeds_enabled_count(void);

/* ---- editing (in RAM; call feeds_save to persist) ---- */
int  feeds_add(const char *url, const char *name);      /* 1 ok, 0 if full/empty/dup */
int  feeds_remove(int i);                               /* 1 ok */
int  feeds_set(int i, const char *url, const char *name);/* replace fields; 1 ok */
int  feeds_toggle(int i);                               /* flip enabled; new state, -1 err */
void feeds_clear(void);

/* replace the list with the 10 built-in reputable world English news feeds. */
void feeds_seed_defaults(void);

/* ---- persistence ---- */
int  feeds_load(const char *path);       /* 0 if read (even empty), -1 if not found */
int  feeds_save(const char *path);       /* 0 / -1 */
/* load from path; if it is missing or empty, seed the defaults and save it.
 * Returns 1 if defaults were seeded, 0 if an existing list was loaded. */
int  feeds_load_or_seed(const char *path);

/* short display label from a feed URL's host (scheme + leading "www." stripped). */
void feeds_host_label(const char *url, char *out, int cap);

#endif
