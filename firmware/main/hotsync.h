/* hotsync.h -- run a sync to iCloud on a background task (U7).
 * Isolated + defensive: failures set an error status, never crash the UI. */
#ifndef HOTSYNC_H
#define HOTSYNC_H

void        hotsync_start(void);     /* kick off a sync if not already running */
int         hotsync_busy(void);      /* 1 while a sync is in progress */
const char *hotsync_status(void);    /* latest status/result line */
int         hotsync_progress(void);  /* coarse 0..100, or -1 when idle */

/* ---- collection discovery (Preferences "Discover collections") -----------
 * Brings Wi-Fi up, walks the iCloud CalDAV + CardDAV homes, and collects the
 * account's calendars / reminders lists / address books so the user can pick
 * one per role instead of pasting UUID paths. Runs on the same background task
 * slot as the sync (only one at a time); the UI polls hotsync_status(). */
typedef struct {
    char href[192];   /* collection path (no leading/trailing slash), as stored in config */
    char name[64];    /* display name                                                     */
    int  kind;        /* 'c' = calendar/reminders (CalDAV), 'a' = address book (CardDAV)   */
} DiscColl;

void            hotsync_discover_start(void);  /* begin a discovery run (no-op if busy) */
int             hotsync_discover_busy(void);   /* 1 while discovering */
int             hotsync_discover_done(void);   /* 1 once a run has finished (results valid) */
int             hotsync_discover_count(void);  /* number of collections found */
const DiscColl *hotsync_discover_get(int i);   /* i in [0,count); NULL if out of range */

#endif
