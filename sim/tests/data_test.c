/* data_test.c -- the device's data layer (firmware/main/data.c): Memos.
 * Runs the REAL data.c against a scratch /sdcard (make -C sim data).
 *
 * Memos is the only database since Date Book, Address and To Do were archived
 * with the iCloud sync (2026-11-07). There is no demo data: a card with no
 * MemoDB.pdb gets an empty one with the standard categories, and the first
 * save must land in it. A record an older build marked deleted (a tombstone,
 * for a sync) must stay invisible. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "data.h"
#include "palm.h"

static int fails;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } \
                    else printf("  ok: %s\n",(m)); }while(0)

static void countRow(uint32_t uid, const char *p, const char *s, void *ctx){
    (void)uid; (void)p; (void)s; (*(int *)ctx)++;
}
static uint32_t g_first; static char g_first_txt[96];
static void firstRow(uint32_t uid, const char *p, const char *s, void *ctx){
    (void)s; (void)ctx;
    if(!g_first){ g_first = uid; snprintf(g_first_txt, sizeof g_first_txt, "%s", p); }
}
typedef struct { int all, tomb; } Raw;
static int rawCb(const PdbRec *r, int i, void *ctx){
    (void)i; Raw *w = ctx; w->all++; if(r->attr & REC_ATTR_DELETE) w->tomb++; return 0;
}
static Raw raw(void){ Raw w = {0, 0}; pdb_read(data_db_path(APP_MEMO), rawCb, &w); return w; }
static int live(void){ int n = 0; data_memo(countRow, &n); return n; }

int main(void){
    printf("data layer gate (Memos)\n");
    remove(data_db_path(APP_MEMO));
    remove("/sdcard/.demoseed");
    data_init();

    /* ---- a new card: an empty database, no demo records ---- */
    Raw r0 = raw();
    CK(r0.all == 0, "a new card's Memos database exists and is empty (no demo data)");
    CatTable t; memset(&t, 0, sizeof t);
    CK(data_get_categories(APP_MEMO, &t) && !strcmp(t.name[0], "Unfiled")
       && !strcmp(t.name[1], "Business") && !strcmp(t.name[2], "Personal"),
       "...with the standard categories");
    data_init();
    CK(raw().all == 0, "data_init again changes nothing");

    /* ---- save, read back, edit, recategorise ---- */
    CK(data_save_memo(0, 1, "Shopping list\nmilk\neggs"), "a first memo saves");
    uint32_t u1 = data_last_new_uid();
    CK(u1 >= 1 && live() == 1, "...and is listed, with a uid");
    char buf[128];
    CK(data_get_memo(u1, buf, sizeof buf) && !strcmp(buf, "Shopping list\nmilk\neggs"), "read back whole");
    CK(data_record_category(APP_MEMO, u1) == 1, "filed under the category it was saved in");
    CK(data_save_memo(0, 0, "Second"), "a second memo");
    uint32_t u2 = data_last_new_uid();
    CK(u2 > u1 && live() == 2, "a new uid past the last");
    CK(data_save_memo(u1, -1, "Shopping list\nbread"), "edit the first, keeping its category");
    CK(data_get_memo(u1, buf, sizeof buf) && !strcmp(buf, "Shopping list\nbread")
       && data_record_category(APP_MEMO, u1) == 1 && live() == 2, "...replaced, not duplicated");

    /* ---- the category filter ---- */
    data_set_category(1);
    CK(live() == 1, "the list honours the category filter");
    data_set_category(-1);
    g_first = 0; data_memo(firstRow, NULL);
    CK(g_first && g_first_txt[0], "a row is the memo's first line");

    /* ---- delete ---- */
    CK(data_delete(APP_MEMO, u2), "delete a memo");
    Raw r1 = raw();
    CK(live() == 1 && r1.all == 1 && r1.tomb == 0, "it is gone outright: no tombstone");
    CK(!data_get_memo(u2, buf, sizeof buf), "and cannot be read");

    /* ---- a tombstone an older build left (for a sync that no longer runs) ---- */
    { PdbRec r[2];
      static const char *a = "kept", *b = "was deleted for a sync";
      r[0] = (PdbRec){ .attr = 0, .uniqueID = 5, .data = (const uint8_t *)a, .len = 5 };
      r[1] = (PdbRec){ .attr = REC_ATTR_DELETE, .uniqueID = 6, .data = (const uint8_t *)b, .len = (int)strlen(b) + 1 };
      uint8_t ai[APPINFO_SIZE]; int al = appinfo_build(ai, sizeof ai, &t);
      pdb_write_ai(data_db_path(APP_MEMO), "MemoDB", 0x44415441, 0x6D656D6F, ai, al, r, 2); }
    CK(live() == 1, "an older build's tombstone is not listed");
    CK(!data_get_memo(6, buf, sizeof buf) && data_record_category(APP_MEMO, 6) < 0,
       "...nor read, nor given a category");
    CK(data_save_memo(0, 0, "New"), "a save on top of it works");
    CK(data_last_new_uid() == 7, "and takes a uid past the tombstone's");

    printf(fails ? "== data: %d FAILURE(S) ==\n" : "== data: all passed ==\n", fails);
    return fails ? 1 : 0;
}
