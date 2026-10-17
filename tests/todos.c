/* todos.c -- To Do keeps the open tasks and those done lately, against Radicale.
 *
 * A Reminders list ten years old holds thousands of finished tasks. With
 * sync_set_todo_window the server lists only the open VTODOs and those
 * COMPLETED since a date, and the same rule as the calendar window holds: a
 * task not listed leaves the device and is never touched on the server. The
 * server's count is checked after every step.
 *
 * Needs Radicale with palm/cal (run_gates.sh sets it up).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"
#include "radicale_rm.h"

static DavCtx D;
static int fails = 0;
static void CK(int c, const char *m){ printf("  %s %s\n", c ? "ok  " : "FAIL", m); if(!c) fails++; }

#define COLL "palm/cal"
#define MAP  "state/todo-win.map"
#define LPDB "pdb/todo-win.pdb"

typedef struct { char n[64][256]; int c; } NL;
static void nlCb(const char *name, const char *etag, void *ctx){
    (void)etag; NL *l = ctx; if(strstr(name, ".ics") && l->c < 64) snprintf(l->n[l->c++], 256, "%s", name);
}
static int serverCount(void){ NL *l = calloc(1, sizeof *l); dav_list(&D, COLL, nlCb, l); int n = l->c; free(l); return n; }
static void clearColl(void){
    NL *l = calloc(1, sizeof *l); dav_list(&D, COLL, nlCb, l);
    for(int i = 0; i < l->c; i++) test_rm(&D, COLL, l->n[i], NULL);
    free(l); remove(MAP);
}
static void putTodo(const char *name, const char *summary, const char *completed){
    char body[1024];
    snprintf(body, sizeof body, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VTODO\r\nUID:%s\r\nSUMMARY:%s\r\n%s%s%s"
             "X-APPLE-SORT-ORDER:42\r\nEND:VTODO\r\nEND:VCALENDAR\r\n", name, summary,
             completed ? "STATUS:COMPLETED\r\nCOMPLETED:" : "STATUS:NEEDS-ACTION\r\n",
             completed ? completed : "", completed ? "\r\n" : "");
    FILE *f = fopen("state/.td", "wb"); fputs(body, f); fclose(f);
    char fn[300]; snprintf(fn, sizeof fn, "%s.ics", name);
    char etag[160]; int st = 0;
    dav_put(&D, COLL, fn, "text/calendar; charset=utf-8", "state/.td", NULL, etag, sizeof etag, &st);
}
typedef struct { uint32_t uid; uint8_t attr; int len; uint8_t data[PALM_REC_MAX]; } Rec;
static Rec recs[32]; static int nrecs;
static int loadCb(const PdbRec *r, int i, void *c){
    (void)i; (void)c;
    if(nrecs < 32){ Rec *x = &recs[nrecs++]; x->uid = r->uniqueID; x->attr = r->attr; x->len = r->len; memcpy(x->data, r->data, (size_t)r->len); }
    return 0;
}
static int findLocal(const char *desc){
    nrecs = 0; pdb_read(LPDB, loadCb, NULL);
    for(int i = 0; i < nrecs; i++){ Todo t; if(!ToDoUnpack(recs[i].data, recs[i].len, &t) && !strcmp(t.description, desc)) return i; }
    return -1;
}
static int localCount(void){ nrecs = 0; pdb_read(LPDB, loadCb, NULL); return nrecs; }
static void saveLocal(void){
    static PdbRec r[32];
    for(int i = 0; i < nrecs; i++) r[i] = (PdbRec){ .attr = recs[i].attr, .uniqueID = recs[i].uid, .data = recs[i].data, .len = recs[i].len };
    pdb_write(LPDB, "ToDoDB", 0x44415441, 0x746F646F, r, nrecs);
}
static int sync1(SyncStats *s){ memset(s, 0, sizeof *s); return sync_collection(&D, LPDB, LPDB, COLL, KIND_TODO, MAP, POL_SERVER, s); }

int main(void){
    snprintf(D.base, sizeof D.base, "%s", getenv("DAV_BASE") ? getenv("DAV_BASE") : "http://localhost:5232");
    snprintf(D.user, sizeof D.user, "palm"); snprintf(D.pass, sizeof D.pass, "palm");
    clearColl();
    pdb_write(LPDB, "ToDoDB", 0x44415441, 0x746F646F, NULL, 0);

    /* "lately" is the last two weeks, from now */
    time_t now = time(NULL), since = now - 14 * 86400;
    char recent[20], old[20]; struct tm u;
    time_t r = now - 3 * 86400; gmtime_r(&r, &u); strftime(recent, sizeof recent, "%Y%m%dT%H%M%SZ", &u);
    snprintf(old, sizeof old, "20190101T120000Z");
    putTodo("td-open", "Buy milk", NULL);
    putTodo("td-recent", "Filed taxes", recent);
    putTodo("td-old", "Moved house", old);

    printf("== a fresh device takes the open tasks and those done lately ==\n");
    sync_set_todo_window(since);
    SyncStats s; int rc = sync1(&s);
    CK(rc == 2 && s.pullNew == 2, "two come down");
    CK(findLocal("Buy milk") >= 0 && findLocal("Filed taxes") >= 0 && findLocal("Moved house") < 0, "the open one and the recent one");
    CK(serverCount() == 3, "the server still has all three");

    printf("== ticked on the device: it goes up done, with when ==\n");
    int i = findLocal("Buy milk"); Todo t;
    if(i >= 0){ ToDoUnpack(recs[i].data, recs[i].len, &t); t.completed = 1;
                recs[i].len = ToDoPack(recs[i].data, PALM_REC_MAX, &t); saveLocal(); }
    rc = sync1(&s);
    char obj[4096]; int n = dav_get(&D, COLL, "td-open.ics", obj, sizeof obj); if(n < 0) n = 0; obj[n] = 0;
    CK(s.pushMod == 1, "the tick was sent");
    CK(strstr(obj, "STATUS:COMPLETED") && strstr(obj, "COMPLETED:2"), "done, with a COMPLETED time");
    CK(strstr(obj, "X-APPLE-SORT-ORDER:42") != NULL, "and Apple's own properties kept (merged)");
    rc = sync1(&s);
    CK(findLocal("Buy milk") >= 0, "still on the device: it was done lately");

    printf("== done long ago on the phone: it leaves the device, not the server ==\n");
    putTodo("td-recent", "Filed taxes", old);
    rc = sync1(&s);
    CK(findLocal("Filed taxes") < 0 && s.pruned == 1, "pruned from the device");
    CK(serverCount() == 3, "the server still has all three");

    printf("== no window: the whole list ==\n");
    sync_set_todo_window(0);
    rc = sync1(&s);
    CK(localCount() == 3 && s.pullNew == 2, "everything comes down");
    CK(serverCount() == 3, "and nothing changed on the server");

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
