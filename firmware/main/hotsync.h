/* hotsync.h -- run a sync to iCloud on a background task (U7).
 * Isolated + defensive: failures set an error status, never crash the UI. */
#ifndef HOTSYNC_H
#define HOTSYNC_H

void        hotsync_start(void);     /* kick off a sync if not already running */
int         hotsync_busy(void);      /* 1 while a sync is in progress */
const char *hotsync_status(void);    /* latest status/result line */
int         hotsync_progress(void);  /* coarse 0..100, or -1 when idle */

/* ---- cancelling a run -----------------------------------------------------
 * Ask the running sync to stop. This is a REQUEST, not a kill: it raises a flag
 * that the sync task reads at points where stopping is safe, and returns
 * immediately. The task is never suspended or deleted from outside, because it
 * can be most of the way through writing a record when the user presses the
 * button, and a half-written PDB on the card is a worse outcome than a sync that
 * takes another few seconds to notice.
 *
 * Safe points are between collections and between the major stages (news,
 * weather) -- never inside one collection's merge, and never mid-write. So the
 * worst case is one collection's latency, not instant.
 *
 * A cancelled run finishes tidily: Wi-Fi comes down, the status reads as
 * cancelled rather than failed, and whatever HAD already synced stays synced.
 * hotsync_cancelled() reports that the last completed run ended this way, which
 * is what lets the UI say "Cancelled" instead of leaving the user to wonder
 * whether it broke. */
void hotsync_cancel(void);           /* ask a running sync to stop; no-op if idle */
int  hotsync_cancel_pending(void);   /* 1 once asked, until the run actually ends */
int  hotsync_cancelled(void);        /* 1 if the LAST finished run was cancelled  */

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

/* ---- Wi-Fi scan (Settings ▸ Wi-Fi, "choose a nearby network") -------------
 * An SSID is a value you cannot be asked to type: it is case-sensitive, often
 * has punctuation in it, and getting it wrong fails exactly like a wrong
 * password does. So the device looks, and the user taps. Runs on the same
 * background task slot as a sync (one at a time), brings the radio up WITHOUT
 * associating, and leaves it down again; the UI polls wifi_scan_busy().
 *
 * Results are strongest-first and de-duplicated by name, because one network on
 * two bands is one network to the person choosing it. */
#define WIFI_SCAN_MAX 12                       /* what one picker screen can use */
typedef struct {
    char ssid[33];    /* 32 chars + NUL, the 802.11 maximum */
    int  rssi;        /* dBm; -50 is across the room, -85 is marginal */
    int  secure;      /* 1 if the AP advertises anything but open */
} WifiAP;

void          wifi_scan_start(void);  /* begin a scan (no-op if the slot is busy) */
int           wifi_scan_busy(void);   /* 1 while scanning */
int           wifi_scan_done(void);   /* 1 once a run has finished (results valid) */
int           wifi_scan_count(void);  /* networks found, 0..WIFI_SCAN_MAX */
const WifiAP *wifi_scan_get(int i);   /* i in [0,count); NULL out of range */

void            hotsync_discover_start(void);  /* begin a discovery run (no-op if busy) */
int             hotsync_discover_busy(void);   /* 1 while discovering */
int             hotsync_discover_done(void);   /* 1 once a run has finished (results valid) */
int             hotsync_discover_count(void);  /* number of collections found */
const DiscColl *hotsync_discover_get(int i);   /* i in [0,count); NULL if out of range */

#endif
