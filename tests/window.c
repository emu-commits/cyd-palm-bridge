/* window.c -- a calendar synced through a time window (sync_set_window).
 *
 * A real account holds years of events; the device keeps only a window of
 * them. The server lists the window (a CalDAV time-range query, which it
 * applies to recurrences too), only those objects are fetched -- in batches --
 * and the rest stay on the server.
 *
 * The rule this gate exists for: AN OBJECT LEAVING THE WINDOW IS NOT A
 * DELETION. Its unchanged copy leaves the device and nothing is sent. A copy
 * the device changed is checked against the server first, so an edit still
 * reaches the server and a tombstone still deletes. The server count is
 * checked after every step: nothing may ever vanish from it that the device
 * didn't explicitly delete.
 *
 * Needs Radicale on localhost:5232 with palm/cal (run_gates.sh sets it up).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"

static DavCtx D;
static int fails = 0;
static void CK(int c, const char *m){ if(!c){ fails++; printf("  FAIL: %s\n", m); } }

#define COLL "palm/cal"
#define MAP  "state/win.map"
#define LPDB "pdb/win.pdb"
#define DAY  86400LL

typedef struct { char n[256][256]; int c; } NL;
static void nlCb(const char *name, const char *etag, void *ctx){
    (void)etag; NL *l = ctx; if(strstr(name, ".ics") && l->c < 256) snprintf(l->n[l->c++], 256, "%s", name);
}
static void clearColl(void){
    NL *l = calloc(1, sizeof *l); dav_list(&D, COLL, nlCb, l);
    for(int i = 0; i < l->c; i++) dav_delete(&D, COLL, l->n[i], NULL);
    remove(MAP); free(l);
}
static int serverCount(void){ NL *l = calloc(1, sizeof *l); dav_list(&D, COLL, nlCb, l); int n = l->c; free(l); return n; }

/* ---- the local database, small enough to hold whole in a test ---- */
typedef struct { uint32_t uid; uint8_t attr; int len; uint8_t data[PALM_REC_MAX]; } Rec;
static Rec recs[64]; static int nrecs;
static int loadCb(const PdbRec *r, int i, void *c){
    (void)i; (void)c;
    if(nrecs < 64){ Rec *x = &recs[nrecs++]; x->uid = r->uniqueID; x->attr = r->attr; x->len = r->len; memcpy(x->data, r->data, (size_t)r->len); }
    return 0;
}
static void loadLocal(void){ nrecs = 0; pdb_read(LPDB, loadCb, NULL); }
static void saveLocal(void){
    static PdbRec r[64];
    for(int i = 0; i < nrecs; i++) r[i] = (PdbRec){ .attr = recs[i].attr, .uniqueID = recs[i].uid, .data = recs[i].data, .len = recs[i].len };
    pdb_write(LPDB, "DatebookDB", 0x44415441, 0x64617465, r, nrecs);
}
static int localCount(void){ loadLocal(); return nrecs; }
/* the local record whose description is `d`, or -1 */
static int findLocal(const char *d){
    loadLocal();
    for(int i = 0; i < nrecs; i++){ Appt a; if(!ApptUnpack(recs[i].data, recs[i].len, &a) && !strcmp(a.description, d)) return i; }
    return -1;
}

/* ---- events, dated relative to today ---- */
static time_t today0;                       /* local midnight today */
static void dateOf(int offset, int *y, int *m, int *d){
    time_t t = today0 + (time_t)offset * DAY + 12 * 3600; struct tm tm; localtime_r(&t, &tm);
    *y = tm.tm_year + 1900; *m = tm.tm_mon + 1; *d = tm.tm_mday;
}
static void putEvent(const char *name, uint32_t uid, int offset, int weekly, const char *desc){
    Appt a; memset(&a, 0, sizeof a);
    a.hasTime = 1; a.sH = 9; a.eH = 10;
    dateOf(offset, &a.year, &a.month, &a.day);
    if(weekly){
        time_t t = today0 + (time_t)offset * DAY + 12 * 3600; struct tm tm; localtime_r(&t, &tm);
        a.hasRepeat = 1; a.repeatType = repeatWeekly; a.repeatFreq = 1; a.repeatForever = 1;
        a.repeatOn = 1 << tm.tm_wday;
    }
    snprintf(a.description, sizeof a.description, "%s", desc);
    char v[4096]; ical_emit(v, sizeof v, &a, uid);
    char obj[5000]; int on = snprintf(obj, sizeof obj, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//t//EN\r\n%sEND:VCALENDAR\r\n", v);
    FILE *f = fopen("state/.wbody", "wb"); fwrite(obj, 1, (size_t)on, f); fclose(f);
    char etag[160]; int st = 0;
    dav_put(&D, COLL, name, "text/calendar; charset=utf-8", "state/.wbody", NULL, etag, sizeof etag, &st);
}
/* the server copy of `name` contains `text` */
static int serverHas(const char *name, const char *text){
    static char buf[65536]; int n = dav_get(&D, COLL, name, buf, sizeof buf);
    return n > 0 && strstr(buf, text) != NULL;
}

static void window(int from, int to){ sync_set_window((long long)today0 + from * DAY, (long long)today0 + to * DAY); }
static int sync1(SyncStats *s){ memset(s, 0, sizeof *s); return sync_collection(&D, LPDB, LPDB, COLL, KIND_CAL, MAP, POL_SERVER, s); }

int main(void){
    snprintf(D.base, sizeof D.base, "%s", getenv("DAV_BASE") ? getenv("DAV_BASE") : "http://localhost:5232");
    snprintf(D.user, sizeof D.user, "palm"); snprintf(D.pass, sizeof D.pass, "palm");
    { time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); tm.tm_hour = tm.tm_min = tm.tm_sec = 0; tm.tm_isdst = -1; today0 = mktime(&tm); }

    clearColl();
    putEvent("w-a.ics", 7001, 2,    0, "Soon A");        /* in the near window      */
    putEvent("w-e.ics", 7005, 6,    0, "Soon E");        /* in the near window      */
    putEvent("w-b.ics", 7002, 60,   0, "Later B");       /* two months out          */
    putEvent("w-c.ics", 7003, -30,  0, "Past C");        /* a month ago             */
    putEvent("w-d.ics", 7004, -364, 1, "Weekly D");      /* weekly since a year ago */
    CK(serverCount() == 5, "setup: five events on the server");
    remove(LPDB); remove(MAP);
    pdb_write(LPDB, "DatebookDB", 0x44415441, 0x64617465, NULL, 0);

    printf("== a fresh device takes only the window ==\n");
    window(-1, 14);
    SyncStats s; int rc = sync1(&s);
    int batched = 0, single = 0; sync_fetch_counts(&batched, &single);
    printf("  rc=%d pullNew=%d batched=%d single=%d\n", rc, s.pullNew, batched, single);
    CK(rc == 3 && s.pullNew == 3, "three events in the window: A, E, and the weekly D");
    CK(findLocal("Soon A") >= 0 && findLocal("Soon E") >= 0 && findLocal("Weekly D") >= 0, "the right three");
    CK(findLocal("Later B") < 0 && findLocal("Past C") < 0, "nothing outside it");
    CK(batched == 3 && single == 0, "fetched in a batch, not one GET each");
    CK(serverCount() == 5, "the server still holds all five");

    printf("== the same window again: nothing to do ==\n");
    rc = sync1(&s);
    int ops = s.pushNew + s.pushMod + s.pushDel + s.pullNew + s.pullMod + s.pullDel + s.conflicts;
    CK(rc == 3 && ops == 0 && s.unchanged == 3 && s.pruned == 0, "a no-op");

    printf("== time moves on: what left the window leaves the device, not the server ==\n");
    window(50, 70);
    rc = sync1(&s);
    printf("  rc=%d pullNew=%d pruned=%d pushDel=%d\n", rc, s.pullNew, s.pruned, s.pushDel);
    CK(s.pruned == 2, "A and E pruned");
    CK(s.pullNew == 1 && findLocal("Later B") >= 0, "B pulled in");
    CK(findLocal("Weekly D") >= 0, "the weekly event stays: it recurs in this window too");
    CK(s.pushDel == 0 && serverCount() == 5, "NOTHING deleted on the server");

    printf("== an edit to an event that then leaves the window still reaches the server ==\n");
    window(-1, 14); sync1(&s);                           /* A and E back, B pruned */
    CK(findLocal("Soon A") >= 0 && findLocal("Later B") < 0, "back in the near window");
    int ia = findLocal("Soon A");
    { Appt a; ApptUnpack(recs[ia].data, recs[ia].len, &a);
      snprintf(a.description, sizeof a.description, "Soon A, edited");
      recs[ia].len = ApptPack(recs[ia].data, PALM_REC_MAX, &a); saveLocal(); }
    window(50, 70);
    rc = sync1(&s);
    printf("  rc=%d pushMod=%d pruned=%d\n", rc, s.pushMod, s.pruned);
    CK(s.pushMod == 1, "the edit was pushed, although A is outside the window");
    CK(serverHas("w-a.ics", "edited"), "the server copy carries the edit (iCalendar escapes the comma)");
    rc = sync1(&s);
    CK(findLocal("Soon A, edited") < 0, "and at the next sync it leaves the device");
    CK(serverCount() == 5, "still five on the server");

    printf("== a tombstone for an event outside the window still deletes it ==\n");
    window(-1, 14); sync1(&s);
    int ie = findLocal("Soon E");
    CK(ie >= 0, "E is back");
    if(ie >= 0){ recs[ie].attr |= REC_ATTR_DELETE; saveLocal(); }
    window(50, 70);
    rc = sync1(&s);
    printf("  rc=%d pushDel=%d\n", rc, s.pushDel);
    CK(s.pushDel == 1 && serverCount() == 4, "E deleted on the server, and only E");

    printf("== a record made on the device outside the window goes up, then leaves ==\n");
    window(-1, 14); sync1(&s);
    loadLocal();
    { Appt a; memset(&a, 0, sizeof a); a.hasTime = 1; a.sH = 14; a.eH = 15;
      dateOf(100, &a.year, &a.month, &a.day); snprintf(a.description, sizeof a.description, "Far F");
      Rec *x = &recs[nrecs++]; x->uid = 4242; x->attr = 0; x->len = ApptPack(x->data, PALM_REC_MAX, &a); saveLocal(); }
    rc = sync1(&s);
    CK(s.pushNew == 1 && serverCount() == 5, "pushed: the server has it");
    rc = sync1(&s);
    CK(s.pruned == 1 && findLocal("Far F") < 0, "then pruned from the device");
    CK(serverCount() == 5, "and kept on the server");

    printf("== a window with nothing in it empties the device, and that's allowed ==\n");
    window(2000, 2001);
    rc = sync1(&s);
    printf("  rc=%d pruned=%d local=%d\n", rc, s.pruned, localCount());
    CK(rc == 0 && localCount() == 0, "the device is empty (not refused as a glitch)");
    CK(serverCount() == 5, "and the server still holds everything");

    printf("== no window: the whole collection, as before ==\n");
    sync_set_window(0, 0);
    rc = sync1(&s);
    CK(rc == 5 && s.pullNew == 5, "every event comes down");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
