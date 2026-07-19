/* hotsync_stub.c -- simulator stand-in for firmware/main/hotsync.c.
 *
 * The sim has no Wi-Fi/TLS, so the iCloud PIM sync can't run here. But the RSS
 * reader's feed fetch CAN be demoed offline: "Sync Now" rebuilds the News store
 * from the enabled feeds (Preferences > News feeds) with placeholder articles, so
 * the whole News flow -- add/enable a feed, HotSync, swipe the reader -- works in
 * the web emulator exactly as it will on device (where the same step fetches the
 * feeds' real RSS). Discovery stays disabled (it needs the network). */
#include "hotsync.h"
#include "feeds.h"
#include "news.h"
#include <stdio.h>
#include <string.h>

static char s_status[80] = "Tap Sync Now to fetch news";

/* offline "fetch": fill the News store with a few sample items per enabled feed. */
static void sim_fetch_news(void){
    int en = feeds_enabled_count();
    if(en == 0){ snprintf(s_status, sizeof s_status, "No feeds enabled (Prefs > News feeds)"); return; }
    if(!news_begin()){ snprintf(s_status, sizeof s_status, "News store error"); return; }
    int stored = 0, nf = feeds_count();
    for(int i=0; i<nf && stored < 30; i++){
        const Feed *f = feeds_get(i);
        if(!f || !f->enabled) continue;
        for(int k=1; k<=3 && stored < 30; k++){
            char title[128], body[320];
            snprintf(title, sizeof title, "%s: sample story %d", f->name, k);
            snprintf(body,  sizeof body,
                     "Placeholder article %d from \"%s\".\n\n"
                     "The web emulator has no network, so HotSync fills the News reader "
                     "with sample items to demo the flow. On the device, this same step "
                     "streams the feed's real RSS to the SD card. Swipe up for the next "
                     "story, down for the previous.", k, f->name);
            if(news_add(f->name, title, body, 0)) stored++;
        }
    }
    news_commit();
    snprintf(s_status, sizeof s_status, "News updated: %d items from %d feeds", stored, en);
}

void        hotsync_start(void)    { sim_fetch_news(); }
int         hotsync_busy(void)     { return 0; }
const char *hotsync_status(void)   { return s_status; }
int         hotsync_progress(void) { return -1; }

void hotsync_discover_start(void)  { s_status[0]=0; snprintf(s_status,sizeof s_status,"Discovery is disabled in the simulator"); }
int  hotsync_discover_busy(void)   { return 0; }
int  hotsync_discover_done(void)   { return 1; }   /* "finished" with zero results */
int  hotsync_discover_count(void)  { return 0; }
const DiscColl *hotsync_discover_get(int i){ (void)i; return 0; }
