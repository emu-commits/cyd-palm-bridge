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
#include <time.h>
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
    /* an edit form saved on the way out (the lock, Home) reopens on its
     * record, so a new one's uid has to be known once the card assigned it */
    CK(data_last_new_uid() == g_first, "data_last_new_uid() names the record just created");
    CK(data_save_todo(g_first, -1, &t) && data_last_new_uid() == g_first,
       "saving it again changes nothing there (an edit, not a new record)");

    /* ---- a real account's size: every rewrite streams --------------------
     * Edits, deletes and category changes used to go through a 12 KB,
     * 64-record arena and write back what fit. Two thousand contacts: an edit
     * and a delete past the old cap, and a category change, must keep every
     * one, change the right one in place, and append nothing. */
    printf("-- 2000 contacts --\n");
    /* Address is read-only on the device while it syncs, and only then; a
     * Date Book that syncs stays editable. The data layer itself still
     * writes contacts: the rewrites below are what a sync's output uses. */
    CK(!data_app_readonly(APP_ADDR), "Address with no account collection: editable");
    snprintf(c->card_coll, sizeof c->card_coll, "card");
    CK(data_app_readonly(APP_ADDR), "Address that syncs: read-only");
    CK(!data_app_readonly(APP_CAL) && data_app_syncs(APP_CAL), "a Date Book that syncs: editable");
    {
        static uint8_t arena[2000 * 64]; static PdbRec rr[2000]; int used = 0;
        for(int i = 0; i < 2000; i++){
            Addr a; memset(&a, 0, sizeof a);
            char nm[32]; snprintf(nm, sizeof nm, "Person %04d", i);
            a.fields[F_name] = AddrIntern(&a, nm);
            a.fields[F_phone1] = AddrIntern(&a, "+1 555 0100");
            int l = AddrPack(arena + used, 64, &a);
            rr[i] = (PdbRec){ .attr = 0, .uniqueID = (uint32_t)(100 + i), .data = arena + used, .len = l };
            used += l;
        }
        pdb_write(data_db_path(APP_ADDR), "AddressDB", 0x44415441, 0x61646472, rr, 2000);
    }
    Raw b0 = raw(APP_ADDR);
    CK(b0.all == 2000, "2000 contacts on the card");
    Addr ea; CK(data_get_addr(1600, &ea), "contact 1600 (past the old 64-record cap) reads");
    ea.fields[F_phone1] = AddrIntern(&ea, "+1 555 0999");
    CK(data_save_addr(1600, -1, &ea), "edit it");
    Raw b1 = raw(APP_ADDR);
    CK(b1.all == 2000, "still 2000: nothing truncated, nothing appended");
    Addr a2; data_get_addr(1600, &a2);
    CK(a2.fields[F_phone1] && !strcmp(a2.fields[F_phone1], "+1 555 0999"), "the edit landed on that contact");
    Addr a3; data_get_addr(2099, &a3);
    CK(a3.fields[F_name] && !strcmp(a3.fields[F_name], "Person 1999"), "and the last contact is untouched");
    CK(data_delete(APP_ADDR, 1700), "delete contact 1700");
    Raw b2 = raw(APP_ADDR);
    CK(b2.all == 2000 && b2.tomb == 1, "a tombstone, and every other contact kept");
    CatTable ct; data_get_categories(APP_ADDR, &ct);
    snprintf(ct.name[5], sizeof ct.name[5], "Clients");
    CK(data_set_categories(APP_ADDR, &ct), "rename a category");
    CK(raw(APP_ADDR).all == 2000, "all 2000 kept through a category change");
    Addr na; memset(&na, 0, sizeof na); na.fields[F_name] = AddrIntern(&na, "Brand New");
    CK(data_save_addr(0, 0, &na), "a new contact");
    CK(raw(APP_ADDR).all == 2001, "appended: 2001");

    /* ---- a repeating event shows on the days it occurs ------------------
     * It used to show on its start date only: a weekly meeting that began in
     * 2019 never appeared in the days a sync window keeps. */
    printf("-- a weekly meeting from 2019 --\n");
    {
        Appt w; memset(&w, 0, sizeof w);
        w.hasTime = 1; w.sH = 9; w.eH = 10; w.year = 2019; w.month = 1; w.day = 7;   /* a Monday */
        w.hasRepeat = 1; w.repeatType = repeatWeekly; w.repeatFreq = 1; w.repeatForever = 1; w.repeatOn = 1 << 1;
        snprintf(w.description, sizeof w.description, "Standup");
        uint8_t pk[512]; int l = ApptPack(pk, sizeof pk, &w);
        PdbRec one = { .attr = 0, .uniqueID = 500, .data = pk, .len = l };
        pdb_write(data_db_path(APP_CAL), "DatebookDB", 0x44415441, 0x64617465, &one, 1);
        int mon = 0, tue = 0;
        data_cal_day(2026, 10, 12, countRow, &mon);
        data_cal_day(2026, 10, 13, countRow, &tue);
        CK(mon == 1 && tue == 0, "on Monday 12 Oct 2026, not Tuesday");
        uint8_t marks[32]; data_cal_month_marks(2026, 10, marks);
        CK(marks[5] && marks[12] && marks[19] && marks[26] && !marks[13], "every Monday of the month dotted");
    }

    printf("-- reminders: alarms that came due in a span --\n");
    {
        /* Wed 12 Mar 2031 09:00, five minutes' notice; the Standup above (every
         * Monday 09:00, no alarm); an hour's notice on a weekly Friday 14:00
         * from 2030; and an untimed event with an alarm, which can't ring */
        Appt one; memset(&one, 0, sizeof one);
        one.hasTime = 1; one.sH = 9; one.eH = 10; one.year = 2031; one.month = 3; one.day = 12;
        one.hasAlarm = 1; one.alarmAdv = 5; one.alarmUnit = 0;
        snprintf(one.description, sizeof one.description, "Dentist");
        CK(data_save_cal(0, 0, &one), "an event with five minutes' notice");
        Appt wk; memset(&wk, 0, sizeof wk);
        wk.hasTime = 1; wk.sH = 14; wk.eH = 15; wk.year = 2030; wk.month = 1; wk.day = 4;   /* a Friday */
        wk.hasRepeat = 1; wk.repeatType = repeatWeekly; wk.repeatFreq = 1; wk.repeatForever = 1; wk.repeatOn = 1 << 5;
        wk.hasAlarm = 1; wk.alarmAdv = 1; wk.alarmUnit = 1;
        snprintf(wk.description, sizeof wk.description, "Review");
        CK(data_save_cal(0, 0, &wk), "a weekly event with an hour's notice");
        Appt nt; memset(&nt, 0, sizeof nt);
        nt.year = 2031; nt.month = 3; nt.day = 12; nt.hasAlarm = 1; nt.alarmAdv = 5;
        snprintf(nt.description, sizeof nt.description, "Untimed");
        CK(data_save_cal(0, 0, &nt), "an untimed event with an alarm");

        struct tm t = {0}; t.tm_year = 2031 - 1900; t.tm_mon = 2; t.tm_mday = 12; t.tm_isdst = -1;
        t.tm_hour = 8; t.tm_min = 55; time_t at = mktime(&t);           /* the Dentist's alarm */
        int n = 0;
        n = 0; data_cal_alarms(at - 3600, at - 1, countRow, &n);
        CK(n == 0, "not before its time");
        n = 0; data_cal_alarms(at - 3600, at, countRow, &n);
        CK(n == 1, "due at start less five minutes (the span's end counts)");
        n = 0; data_cal_alarms(at, at + 600, countRow, &n);
        CK(n == 0, "and only once: a span that starts at it doesn't repeat it");
        /* Friday 14 Mar 2031 14:00 less an hour is 13:00 */
        t.tm_mday = 14; t.tm_hour = 13; t.tm_min = 0; time_t fri = mktime(&t);
        n = 0; data_cal_alarms(fri - 60, fri, countRow, &n);
        CK(n == 1, "a weekly event from 2030 rings on this week's Friday, an hour early");
        n = 0; data_cal_alarms(fri, fri + 3599, countRow, &n);
        CK(n == 0, "nothing in the hour after it");
        /* a device asleep for a month: only the last DATA_ALARM_DAYS days */
        n = 0; data_cal_alarms(fri - 30 * 86400L, fri, countRow, &n);
        CK(n == 2, "a month asleep: the Dentist and this Friday, not four weeks of Fridays");
        n = 0; data_cal_alarms(at, at - 1, countRow, &n);
        CK(n == 0, "an empty span is nothing");
    }

    printf(fails ? "== data: %d FAILURE(S) ==\n" : "== data: all passed ==\n", fails);
    return fails ? 1 : 0;
}
