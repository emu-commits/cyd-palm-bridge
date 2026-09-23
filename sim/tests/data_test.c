/* data_test.c -- the device's data layer: tombstones, and the demo seed's uids.
 * Runs the REAL firmware/main/data.c against a scratch /sdcard (make -C sim data).
 *
 * The sync engine's half of tombstones is gated against Radicale (tests/massdel.c:
 * a tombstone is pushed as a deletion even under the mass-delete guard). This is
 * the device's half, which nothing exercised: a record deleted in an app that
 * syncs must STAY in the database with the delete bit set -- or the engine has
 * nothing to push -- and must VANISH from everything the user can see, or the
 * delete looks like it did nothing. And an app that does not sync must not keep
 * tombstones at all, because there is no sync that would ever clear them.
 *
 * Also the demo seed: HotSync holds uids 1..n back from the push, so a record the
 * USER creates must never be given one of those uids -- even after every sample
 * has been deleted and the database's highest uid is below n. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "data.h"
#include "appcfg.h"
#include "palm.h"

static int fails;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } \
                    else printf("  ok: %s\n",(m)); }while(0)

static void countRow(uint32_t uid, const char *p, const char *s, void *ctx){
    (void)uid; (void)p; (void)s; (*(int *)ctx)++;
}
static uint32_t g_first;
static void firstRow(uint32_t uid, const char *p, const char *s, void *ctx){
    (void)p; (void)s; (void)ctx; if(!g_first) g_first = uid;
}
typedef struct { int all, tomb; } Raw;
static int rawCb(const PdbRec *r, int i, void *ctx){
    (void)i; Raw *w = ctx; w->all++; if(r->attr & REC_ATTR_DELETE) w->tomb++; return 0;
}
static Raw raw(int app){ Raw w = {0, 0}; pdb_read(data_db_path(app), rawCb, &w); return w; }
static int live(void (*it)(data_row_cb, void *)){ int n = 0; it(countRow, &n); return n; }

int main(void){
    printf("data layer gate\n");
    remove(data_db_path(APP_CAL)); remove(data_db_path(APP_MEMO));
    remove(data_db_path(APP_TODO)); remove(data_db_path(APP_ADDR));
    remove("/sdcard/.demoseed");
    data_seed_if_empty();

    /* ---- an app that syncs: the delete leaves a tombstone ---- */
    Config *c = appcfg_mut();
    snprintf(c->dav_user, sizeof c->dav_user, "someone");
    snprintf(c->cal_coll, sizeof c->cal_coll, "home/calendar");
    c->todo_coll[0] = 0;

    int n0 = live(data_datebook);
    Raw r0 = raw(APP_CAL);
    CK(n0 > 1 && r0.all == n0 && r0.tomb == 0, "the seeded Date Book is all live");
    g_first = 0; data_datebook(firstRow, NULL);
    uint32_t gone = g_first;
    CK(data_delete(APP_CAL, gone), "delete an event");
    Raw r1 = raw(APP_CAL);
    CK(r1.all == n0 && r1.tomb == 1, "it STAYS in the database, flagged as deleted");
    CK(live(data_datebook) == n0 - 1, "and is gone from the list");
    Appt a;
    CK(!data_get_cal(gone, &a), "and from the detail lookup");
    char det[256];
    CK(!data_detail(APP_CAL, gone, det, sizeof det), "and from the detail text");

    /* ---- an app that does NOT sync: no tombstone ---- */
    int t0 = live(data_todo);
    g_first = 0; data_todo(firstRow, NULL);
    CK(data_delete(APP_TODO, g_first), "delete a to do (no To Do collection set)");
    Raw rt = raw(APP_TODO);
    CK(rt.all == t0 - 1 && rt.tomb == 0, "it is removed outright -- nothing would ever sync it away");

    /* ---- Memo never syncs ---- */
    int m0 = live(data_memo);
    g_first = 0; data_memo(firstRow, NULL);
    CK(data_delete(APP_MEMO, g_first), "delete a memo");
    Raw rm = raw(APP_MEMO);
    CK(rm.all == m0 - 1 && rm.tomb == 0, "Memo never keeps a tombstone");

    /* ---- the demo seed's uid range is never reused ---- */
    int dn = data_demo_count(APP_TODO);
    CK(dn > 0, "the To Do seed is recorded in the manifest");
    /* delete every remaining seeded to do, so the highest uid left is below dn */
    for(;;){ g_first = 0; data_todo(firstRow, NULL); if(!g_first) break; data_delete(APP_TODO, g_first); }
    CK(live(data_todo) == 0, "every sample to do deleted");
    Todo t; memset(&t, 0, sizeof t);
    snprintf(t.description, sizeof t.description, "mine");
    CK(data_save_todo(0, 0, &t), "a new to do of the user's own");
    g_first = 0; data_todo(firstRow, NULL);
    CK(g_first > (uint32_t)dn, "gets a uid ABOVE the demo range, so HotSync will push it");

    printf(fails ? "== data: %d FAILURE(S) ==\n" : "== data: all passed ==\n", fails);
    return fails ? 1 : 0;
}
