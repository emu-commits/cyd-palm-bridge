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

/* Cancel is a no-op here and must stay one. The sim's "sync" is a synchronous
 * function call that has already finished by the time any button could be
 * pressed, so there is never a run to stop -- hotsync_busy() is 0, which makes
 * the UI keep the button as "Sync Now" and never open the confirmation. The
 * cancel PATH is therefore device-only; what the simulator gates is that the
 * button still reads "Sync Now" and still syncs when nothing is running. */
void hotsync_cancel(void)          { }
int  hotsync_cancel_pending(void)  { return 0; }
int  hotsync_cancelled(void)       { return 0; }

/* The Wi-Fi scan DOES run here, against a fixed pretend neighbourhood. The radio
 * is the only part the simulator cannot have; the flow built on top of it --
 * look, list what was found, tap one, then type only the password -- is the
 * whole point of the Wi-Fi wizard and is exactly the part worth gating. The
 * names are deliberately awkward (a space, a hyphen, mixed case, one open
 * network) because that is what makes typing an SSID a bad idea in the first
 * place, and the picker has to render them.
 *
 * Synchronous, like the stub's "sync": busy is never observably 1, so the UI's
 * polling path has to tolerate a run that is already finished when it looks. */
static WifiAP s_aps[] = {
    { "Copper Beech",   -41, 1 },
    { "Copper Beech 5G",-44, 1 },          /* same name+suffix: NOT deduped, by design */
    { "BT-WiFi-X",      -57, 1 },
    { "eduroam",        -68, 1 },
    { "Cafe Guest",     -74, 0 },          /* open */
    { "VM8842273",      -81, 1 },
};
static int s_scanned;

void wifi_scan_start(void){
    s_scanned = 1;
    snprintf(s_status, sizeof s_status, "Tap a network");
}
int  wifi_scan_busy(void){ return 0; }
int  wifi_scan_done(void){ return s_scanned; }
int  wifi_scan_count(void){
    return s_scanned ? (int)(sizeof s_aps / sizeof s_aps[0]) : 0;
}
const WifiAP *wifi_scan_get(int i){
    return (s_scanned && i >= 0 && i < wifi_scan_count()) ? &s_aps[i] : 0;
}

/* Discovery, like the Wi-Fi scan, runs here against a pretend account. It used
 * to answer "disabled in the simulator" with zero results, which meant the one
 * screen that exists to stop people pasting UUID paths was never once rendered
 * by the gate. The hrefs are shaped like the real thing -- a numeric principal
 * and a UUID -- because their whole point is that they are unreadable and must
 * never be typed; the NAMES are what the user picks from. Only the network is
 * missing here, and the network is not the part that gets designed wrong. */
static const DiscColl s_disc[] = {
    { "1234567890/calendars/home",                          "Home",        'c' },
    { "1234567890/calendars/work",                          "Work",        'c' },
    { "1234567890/calendars/1A2B3C4D-5E6F-7089-ABCD-EF0123456789", "Reminders", 'c' },
    { "1234567890/carddavhome/card",                        "All Contacts",'a' },
};
static int s_disc_ran;

void hotsync_discover_start(void){
    s_disc_ran = 1;
    snprintf(s_status, sizeof s_status, "Found %d collections",
             (int)(sizeof s_disc / sizeof s_disc[0]));
}
int  hotsync_discover_busy(void)   { return 0; }
int  hotsync_discover_done(void)   { return s_disc_ran; }
int  hotsync_discover_count(void)  {
    return s_disc_ran ? (int)(sizeof s_disc / sizeof s_disc[0]) : 0;
}
const DiscColl *hotsync_discover_get(int i){
    return (s_disc_ran && i >= 0 && i < hotsync_discover_count()) ? &s_disc[i] : 0;
}
