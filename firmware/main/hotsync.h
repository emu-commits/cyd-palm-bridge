/* hotsync.h -- run a sync to iCloud on a background task (U7).
 * Isolated + defensive: failures set an error status, never crash the UI. */
#ifndef HOTSYNC_H
#define HOTSYNC_H

void        hotsync_start(void);     /* kick off a sync if not already running */
int         hotsync_busy(void);      /* 1 while a sync is in progress */
const char *hotsync_status(void);    /* latest status/result line */

#endif
