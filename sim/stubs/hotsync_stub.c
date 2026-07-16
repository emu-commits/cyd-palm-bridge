/* hotsync_stub.c -- simulator stand-in for firmware/main/hotsync.c.
 *
 * The sim has no Wi-Fi/TLS; HotSync and Discover report themselves disabled
 * instead of running. The status strings feed the real UI screens, so the
 * HotSync screen and the Preferences Discover flow render and navigate exactly
 * as on device -- they just never start a network task. (Real sync in the
 * browser via fetch() is the plan's S5 stretch goal.) */
#include "hotsync.h"

static const char *s_status = "Sync is disabled in the simulator";

void        hotsync_start(void)    { s_status = "Sync is disabled in the simulator"; }
int         hotsync_busy(void)     { return 0; }
const char *hotsync_status(void)   { return s_status; }
int         hotsync_progress(void) { return -1; }

void hotsync_discover_start(void)  { s_status = "Discovery is disabled in the simulator"; }
int  hotsync_discover_busy(void)   { return 0; }
int  hotsync_discover_done(void)   { return 1; }   /* "finished" with zero results */
int  hotsync_discover_count(void)  { return 0; }
const DiscColl *hotsync_discover_get(int i){ (void)i; return 0; }
