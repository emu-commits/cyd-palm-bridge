/* data.c -- read PDB records from the SD card and hand them to the UI as display
 * strings. Also seeds demo PDBs so the views have content before a HotSync.
 * Uses the shared codec (bridge component) directly; one record in RAM at a time.
 */
#include "data.h"
#include "palm.h"
#include "appinfo.h"
#include "safefile.h"
#include "appcfg.h"       /* which apps sync: decides whether a delete tombstones */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_CAL  "/sdcard/DatebookDB.pdb"
#define DB_ADDR "/sdcard/AddressDB.pdb"
#define DB_TODO "/sdcard/ToDoDB.pdb"
#define DB_MEMO "/sdcard/MemoDB.pdb"
/* records the demo seed so "Remove demo data" can delete exactly what was
 * seeded (per-app record count; seeds always take uniqueIDs 1..nr, and new/synced
 * records get max+1, so 1..nr uniquely identifies the demo rows). */
#define DEMO_MANIFEST "/sdcard/.demoseed"

static const char *db_path(int app){
    return app==APP_CAL?DB_CAL : app==APP_ADDR?DB_ADDR : app==APP_TODO?DB_TODO : DB_MEMO;
}
const char *data_db_path(int app){ return db_path(app); }
/* ---- tombstones --------------------------------------------------------------
 * A record deleted in an app that SYNCS is not dropped from its database: it
 * stays, with the Palm delete bit set, until the next sync has pushed the
 * deletion (the engine then leaves it out). That is what lets the sync engine
 * tell a user's delete -- still present, flagged -- from a card that read short,
 * where the record is simply absent; before this the two were the same shape,
 * and the mass-delete guard had to guess.
 *
 * Every reader the UI uses goes through live_read(), which hides them. Only the
 * rewriters (which must carry tombstones through a save) and the sync engine
 * see the raw database. */
typedef struct { pdb_rec_cb cb; void *ctx; } Live;
static int liveCb(const PdbRec *r, int i, void *ctx){
    Live *l = ctx;
    if(r->attr & REC_ATTR_DELETE) return 0;
    return l->cb(r, i, l->ctx);
}
static int live_read(const char *path, pdb_rec_cb cb, void *ctx){
    Live l = { cb, ctx };
    return pdb_read(path, liveCb, &l);
}

static int file_exists(const char *path){
    FILE *f = fopen(path, "rb");
    if(f){ fclose(f); return 1; }
    return 0;
}

/* shared demo category table (Unfiled/Business/Personal) written into each PDB */
static int build_appinfo(uint8_t *ai){
    CatTable t; memset(&t,0,sizeof t);
    strcpy(t.name[0],"Unfiled"); strcpy(t.name[1],"Business"); strcpy(t.name[2],"Personal");
    return appinfo_build(ai, APPINFO_SIZE, &t);
}

/* current category filter for list views: -1 = All, else a category index */
static int g_catfilter = -1;
void data_set_category(int cat){ g_catfilter = cat; }
int  data_get_category(void){ return g_catfilter; }

int data_get_categories(int app, CatTable *t){
    uint8_t ai[512]; int al = pdb_read_appinfo(db_path(app), ai, sizeof ai);
    if(al<=0) return 0;
    return appinfo_parse(ai, al, t)==0;
}

/* ------------------------- demo seeding -------------------------
 * Enough rows to overflow a list, so a fresh device shows something to scroll.
 * Only a database that doesn't exist yet is seeded, and the seeded records are
 * never pushed to an account (see data_demo_*). */
#define SEEDMAX 32
static int seed_datebook(void){
    uint8_t *arena = malloc(SEEDMAX*96); if(!arena) return 0; PdbRec r[SEEDMAX]; int nr=0, used=0;
    uint8_t ai[APPINFO_SIZE]; int al=build_appinfo(ai);
    struct { const char *s; int mo, d, h; } ev[] = {
        {"Standup",8,3,9},{"1:1 with Sam",8,3,14},{"Groceries",8,4,18},{"Dentist",8,5,10},
        {"Team lunch",8,5,12},{"Code review",8,6,15},{"Gym",8,6,7},{"Call Mom",8,7,19},
        {"Project sync",8,7,11},{"Lunch w/ Ada",8,8,12},{"Haircut",8,9,16},{"Board meeting",8,10,9},
        {"Pay rent",8,11,8},{"Movie night",8,12,20},{"Flight to NYC",8,14,6} };
    int n=(int)(sizeof(ev)/sizeof(ev[0]));
    for(int i=0;i<n && nr<SEEDMAX;i++){
        Appt a; memset(&a,0,sizeof a);
        a.hasTime=1; a.sH=ev[i].h; a.eH=ev[i].h+1;
        a.year=2026; a.month=ev[i].mo; a.day=ev[i].d;
        snprintf(a.description,sizeof a.description,"%s",ev[i].s);
        int l=ApptPack(arena+used,96,&a);
        if(l>0){ r[nr]=(PdbRec){.attr=(uint8_t)(i%3),.uniqueID=(uint32_t)(i+1),.data=arena+used,.len=l}; used+=l; nr++; }
    }
    pdb_write_ai(DB_CAL,"DatebookDB",0x44415441,0x64617465,ai,al,r,nr); free(arena);
    return nr;
}

static int seed_address(void){
    uint8_t *arena = malloc(SEEDMAX*96); if(!arena) return 0; PdbRec r[SEEDMAX]; int nr=0, used=0;
    uint8_t ai[APPINFO_SIZE]; int al=build_appinfo(ai);
    struct { const char *last,*first,*co,*phone; } pc[] = {
        {"Appleseed","Johnny","Acme Corp","555-0100"},{"Bramble","Rosa","Widgets Inc","555-0200"},
        {"Chen","Mei","","555-0300"},{"Okafor","Ada","Globex","555-0400"},
        {"Diaz","Luis","Initech","555-0500"},{"Novak","Petra","","555-0600"},
        {"Kim","Soo","Umbrella","555-0700"},{"Patel","Raj","Hooli","555-0800"},
        {"Silva","Ana","","555-0900"},{"Weber","Tom","Stark Ind","555-1000"},
        {"Yamada","Emi","Cyberdyne","555-1100"},{"Zola","Kofi","","555-1200"} };
    int n=(int)(sizeof(pc)/sizeof(pc[0]));
    for(int i=0;i<n && nr<SEEDMAX;i++){
        Addr a; memset(&a,0,sizeof a);
        a.fields[F_name]=AddrIntern(&a,pc[i].last);
        a.fields[F_firstName]=AddrIntern(&a,pc[i].first);
        if(pc[i].co[0]) a.fields[F_company]=AddrIntern(&a,pc[i].co);
        a.fields[F_phone1]=AddrIntern(&a,pc[i].phone); a.phoneLabel[0]=workLabel;
        int l=AddrPack(arena+used,96,&a);
        if(l>0){ r[nr]=(PdbRec){.attr=(uint8_t)(i%3),.uniqueID=(uint32_t)(i+1),.data=arena+used,.len=l}; used+=l; nr++; }
    }
    pdb_write_ai(DB_ADDR,"AddressDB",0x44415441,0x61646472,ai,al,r,nr); free(arena);
    return nr;
}

static int seed_todo(void){
    uint8_t *arena = malloc(SEEDMAX*96); if(!arena) return 0; PdbRec r[SEEDMAX]; int nr=0, used=0;
    uint8_t ai[APPINFO_SIZE]; int al=build_appinfo(ai);
    struct { const char *s; int pr, done; } td[] = {
        {"Buy stamps",2,0},{"Renew passport",1,0},{"Call plumber",3,0},{"File taxes",1,1},
        {"Water plants",4,0},{"Read RFC 6578",2,1},{"Fix bike tire",3,0},{"Book dentist",2,0},
        {"Reply to Ada",1,0},{"Backup laptop",2,1} };
    int n=(int)(sizeof(td)/sizeof(td[0]));
    for(int i=0;i<n && nr<SEEDMAX;i++){
        Todo t; memset(&t,0,sizeof t);
        t.priority=td[i].pr; t.completed=td[i].done;
        snprintf(t.description,sizeof t.description,"%s",td[i].s);
        int l=ToDoPack(arena+used,96,&t);
        if(l>0){ r[nr]=(PdbRec){.attr=(uint8_t)(i%3),.uniqueID=(uint32_t)(i+1),.data=arena+used,.len=l}; used+=l; nr++; }
    }
    pdb_write_ai(DB_TODO,"ToDoDB",0x44415441,0x746F646F,ai,al,r,nr); free(arena);
    return nr;
}

static int seed_memo(void){
    uint8_t *arena = malloc(SEEDMAX*256); if(!arena) return 0; PdbRec r[SEEDMAX]; int nr=0, used=0;
    uint8_t ai[APPINFO_SIZE]; int al=build_appinfo(ai);
    const char *memos[] = {
        "Shopping list\nMilk\nEggs\nBread\nCoffee",
        "CYD project ideas\n- Palm-style UI\n- iCloud sync\n- battery + case",
        "Meeting notes 8/3\n- discuss sync\n- test on device\n- ship it",
        "Wifi password is on the fridge" };
    int n=(int)(sizeof(memos)/sizeof(memos[0]));
    for(int i=0;i<n && nr<SEEDMAX;i++){
        int len=(int)strlen(memos[i])+1;
        if(used+len > SEEDMAX*256) break;
        memcpy(arena+used, memos[i], len);
        r[nr]=(PdbRec){.attr=(uint8_t)(i%3),.uniqueID=(uint32_t)(i+1),.data=arena+used,.len=len};
        used+=len; nr++;
    }
    pdb_write_ai(DB_MEMO,"MemoDB",0x44415441,0x6D656D6F,ai,al,r,nr); free(arena);
    return nr;
}

static int g_testev_scanned;      /* records examined by the last test-event scan */

/* ---- rewriting a database, a record at a time ------------------------------
 * EVERY REWRITE STREAMS THROUGH THE CARD. Until 2026-10-14 an edit, a delete
 * or a category change read the whole database into a 12 KB, 64-record RAM
 * arena and wrote back what fit: pdb_read stopped at the cap, so editing one
 * contact in a book of two thousand wrote back sixty-four, and an edit to a
 * record past the cap was appended as a new record -- a duplicate the next
 * sync would push. Now each record goes through the streaming writer (PdbW:
 * the bytes to a temp file, 16 bytes of index per record in RAM), and the
 * live file is replaced only if every record the header declares was read and
 * written. Otherwise the file is left exactly as it was and the call fails. */
typedef int (*rw_fate)(const PdbRec *r, uint8_t *attr, const uint8_t **data, int *len, void *ctx);
typedef struct { PdbW *w; rw_fate fate; void *ctx; int seen, err; uint32_t maxuid; } Stream;
static int streamCb(const PdbRec *r, int i, void *c){ (void)i; Stream *s = c;
    s->seen++;
    if(r->uniqueID > s->maxuid) s->maxuid = r->uniqueID;
    uint8_t attr = r->attr; const uint8_t *d = r->data; int len = r->len;
    if(s->fate && s->fate(r, &attr, &d, &len, s->ctx)) return 0;        /* dropped */
    if(pdbw_rec(s->w, r->uniqueID, attr, d, len) != 0){ s->err = 1; return 1; }
    return 0;
}
static int hdr_count(const char *path){
    FILE *f = fopen(path, "rb"); if(!f) return -1;
    unsigned char h[0x4E]; int ok = fread(h, 1, sizeof h, f) == sizeof h; fclose(f);
    return ok ? (int)((h[0x4C] << 8) | h[0x4D]) : -1;
}
/* Rewrite path: fate() decides each record (keep, change, or drop: nonzero);
 * `add` (or NULL) is appended, its uid chosen by pick_uid(maxuid) when 0.
 * ai: the AppInfo to write (NULL keeps the file's own). Returns the record
 * count written, or -1 with the file untouched. */
static int stream_rewrite(const char *path, const char *nm, uint32_t type, uint32_t creator,
                          const uint8_t *ai, int al, rw_fate fate, void *ctx,
                          const PdbRec *add, uint32_t (*pick_uid)(const char *, uint32_t)){
    uint8_t own[512];
    if(!ai){ al = pdb_read_appinfo(path, own, sizeof own); if(al < 0) al = 0; ai = al ? own : NULL; }
    char tmp[64]; snprintf(tmp, sizeof tmp, "%s.rw", path);
    PdbW *w = pdbw_begin(tmp); if(!w) return -1;
    Stream st = { w, fate, ctx, 0, 0, 0 };
    int got = pdb_read(path, streamCb, &st);
    int want = hdr_count(path);
    if(st.err || got < 0 || want < 0 || got != want || st.seen != want){
        fprintf(stderr, "[data] %s: rewrite refused (read %d of %d records%s) -- left as it was\n",
                path, st.seen, want, st.err ? ", the card would not take the copy" : "");
        pdbw_abort(w); return -1;
    }
    if(add){
        uint32_t u = add->uniqueID ? add->uniqueID : (pick_uid ? pick_uid(path, st.maxuid) : st.maxuid + 1);
        if(pdbw_rec(w, u, add->attr, add->data, add->len) != 0){ pdbw_abort(w); return -1; }
    }
    return pdbw_commit(w, path, nm, type, creator, ai, al);
}

/* Migrate PDBs seeded by an older build that had no AppInfo (so the category
 * table was empty and the picker only offered "All"): rewrite with the demo
 * category table, every record kept. */
static void ensure_appinfo(const char *path,const char *nm,uint32_t type,uint32_t creator){
    if(!file_exists(path)) return;
    uint8_t ai[512]; int al=pdb_read_appinfo(path,ai,sizeof ai);
    CatTable t;
    if(al>0 && appinfo_parse(ai,al,&t)==0 && t.name[0][0]) return;   /* already has categories */
    uint8_t nai[APPINFO_SIZE]; int nal=build_appinfo(nai);
    stream_rewrite(path,nm,type,creator,nai,nal,NULL,NULL,NULL,NULL);
}

/* app -> PDB header identity (name/type/creator), matching the seeders. */
static void db_ident(int app, const char **nm, uint32_t *type, uint32_t *creator){
    *type = 0x44415441;   /* 'DATA' */
    switch(app){
        case APP_CAL:  *nm="DatebookDB"; *creator=0x64617465; break;   /* 'date' */
        case APP_ADDR: *nm="AddressDB";  *creator=0x61646472; break;   /* 'addr' */
        case APP_TODO: *nm="ToDoDB";     *creator=0x746F646F; break;   /* 'todo' */
        default:       *nm="MemoDB";     *creator=0x6D656D6F; break;   /* 'memo' */
    }
}

/* write a new category table into the app's PDB AppInfo, preserving every
 * record (the low-nibble category on each record is untouched). Mirrors
 * ensure_appinfo's rewrite; 1 on success, 0 on failure. */
int data_set_categories(int app, const CatTable *t){
    const char *path = db_path(app);
    if(!file_exists(path)) return 0;
    uint8_t nai[APPINFO_SIZE]; int nal = appinfo_build(nai, sizeof nai, t);
    if(nal < 0) return 0;
    const char *nm; uint32_t type, creator; db_ident(app,&nm,&type,&creator);
    return stream_rewrite(path,nm,type,creator,nai,nal,NULL,NULL,NULL,NULL) >= 0;
}

void data_seed_if_empty(void){
    /* Finish any swap a crash interrupted BEFORE asking whether a database
     * exists: on the card, the one state a crash mid-swap can leave is "no
     * DatebookDB.pdb, a complete DatebookDB.pdb.tmp" -- and "does not exist"
     * is exactly what would reseed it with demo rows over the user's own. */
    sf_recover(DB_CAL); sf_recover(DB_ADDR); sf_recover(DB_TODO); sf_recover(DB_MEMO);
    sf_recover(DEMO_MANIFEST);
    /* only seed a DB that doesn't exist yet, so edits + synced data persist */
    int seeded[4] = { -1, -1, -1, -1 };
    if(!file_exists(DB_CAL))  seeded[APP_CAL]  = seed_datebook();
    if(!file_exists(DB_ADDR)) seeded[APP_ADDR] = seed_address();
    if(!file_exists(DB_TODO)) seeded[APP_TODO] = seed_todo();
    if(!file_exists(DB_MEMO)) seeded[APP_MEMO] = seed_memo();
    /* if we seeded anything this boot, record it so the demo rows can be
     * removed as a set before the first HotSync (don't push fake data to iCloud) */
    int any = 0;
    for(int i=0;i<4;i++) if(seeded[i] > 0) any = 1;
    if(any){
        SafeFile sf;
        FILE *f = sf_open(&sf, DEMO_MANIFEST, "wb");
        if(f){
            for(int i=0;i<4;i++) if(seeded[i] > 0) fprintf(f, "%d %d\n", i, seeded[i]);
            sf_commit(&sf, !ferror(f));
        }
    }
    /* backfill categories into PDBs from older builds that lacked AppInfo */
    ensure_appinfo(DB_CAL, "DatebookDB",0x44415441,0x64617465);
    ensure_appinfo(DB_ADDR,"AddressDB", 0x44415441,0x61646472);
    ensure_appinfo(DB_TODO,"ToDoDB",    0x44415441,0x746F646F);
    ensure_appinfo(DB_MEMO,"MemoDB",    0x44415441,0x6D656D6F);
}

/* is the (still-untouched) demo seed present? Drives the "Remove demo data"
 * menu item. The manifest is deleted once the demo rows are removed. */
int data_demo_present(void){ return file_exists(DEMO_MANIFEST); }

/* drop the demo rows: every record whose uniqueID is in 1..maxuid */
typedef struct { uint32_t maxuid; int removed; } DemoRW;
static int demoFate(const PdbRec *r, uint8_t *attr, const uint8_t **d, int *len, void *ctx){
    (void)attr; (void)d; (void)len; DemoRW *w = ctx;
    if(r->uniqueID >= 1 && r->uniqueID <= w->maxuid){ w->removed++; return 1; }
    return 0;
}
/* rewrite one app's PDB once, dropping demo rows (uid 1..nr); returns count removed. */
static int remove_demo_from(int app, uint32_t nr){
    const char *path = db_path(app);
    if(!file_exists(path)) return 0;
    DemoRW w = { nr, 0 };
    const char *nm; uint32_t type, creator; db_ident(app,&nm,&type,&creator);
    if(stream_rewrite(path, nm, type, creator, NULL, 0, demoFate, &w, NULL, NULL) < 0) return 0;
    return w.removed;
}

/* ---- dev "Add test events" cleanup ----------------------------------------
 * The devtools menu seeds 30 appointments carrying an exact marker in their
 * note. They were only ever local -- no sync ever completed while they existed
 * -- but with two-way sync live they would be pushed to the real account on the
 * next HotSync. Matching is the FULL marker string, so a genuine note is never
 * mistaken for one.
 *
 * The rewrite streams through the card and is refused, leaving the file as it
 * was, unless every record the header declares was read (stream_rewrite). */
#define TESTEV_NOTE "generated for streaming-sync test"

/* Pass 1: COUNT, and never stop early.
 *
 * Doing this in one pass -- copying survivors into the rewrite buffer while
 * looking for matches -- means the scan aborts the moment that buffer fills, and
 * any test event living past that point is never even examined. It would then
 * report "none found" for a Date Book that has thirty of them, which is a worse
 * failure than not trying: you would believe the seed was gone and turn on
 * two-way sync. Counting needs no buffer, so pass 1 always sees every record. */
typedef struct { int matched, scanned; } TestScan;
static int testScanCb(const PdbRec *r, int i, void *ctx){ (void)i; TestScan *t=ctx;
    Appt a; t->scanned++;
    if(ApptUnpack(r->data, r->len, &a)==0 && !strcmp(a.note, TESTEV_NOTE)) t->matched++;
    return 0;
}
/* Pass 2: the rewrite, dropping exactly the matches */
static int testFate(const PdbRec *r, uint8_t *attr, const uint8_t **d, int *len, void *ctx){
    (void)attr; (void)d; (void)len; int *dropped = ctx;
    Appt a;
    if(ApptUnpack(r->data, r->len, &a)==0 && !strcmp(a.note, TESTEV_NOTE)){ (*dropped)++; return 1; }
    return 0;
}
int data_testev_scanned(void){ return g_testev_scanned; }

/* Header record count vs what pdb_read can actually walk. They should agree; when
 * they do not the file is truncated, and with two-way sync live that difference
 * would read as "the user deleted these" and be pushed to the server as deletes.
 * Returns the header count (-1 if unreadable) and fills *walked / *bytes. */
int data_db_stat(int app, int *walked, long *bytes){
    const char *path = db_path(app);
    if(walked) *walked = 0;
    if(bytes)  *bytes  = 0;
    FILE *f = fopen(path,"rb"); if(!f) return -1;
    unsigned char h[0x50];
    int ok = fread(h,1,sizeof h,f)==sizeof h;
    if(bytes){ fseek(f,0,SEEK_END); *bytes = ftell(f); }
    fclose(f);
    if(!ok) return -1;
    if(walked){ TestScan t={0,0}; pdb_read(path, testScanCb, &t); *walked = t.scanned; }
    return (int)((h[0x4C]<<8) | h[0x4D]);
}
int data_remove_test_events(void){
    const char *path = db_path(APP_CAL);
    if(!file_exists(path)) return 0;

    TestScan t = { 0, 0 };
    pdb_read(path, testScanCb, &t);
    g_testev_scanned = t.scanned;            /* so the caller can report honestly */
    if(t.matched == 0) return 0;             /* genuinely none: do not touch the file */

    int dropped = 0;
    if(stream_rewrite(path, "DatebookDB", 0x44415441, 0x64617465, NULL, 0, testFate, &dropped, NULL, NULL) < 0)
        return -1;                           /* the file is untouched -- refuse, say so */
    return dropped;
}

/* delete exactly the demo-seeded records (uniqueIDs 1..nr per app from the
 * manifest -- ONE rewrite per app, not per record), then drop the manifest.
 * Returns the number of records removed. User-added / synced records
 * (uniqueID > nr) are never touched. */
int data_demo_count(int app){
    FILE *f = fopen(DEMO_MANIFEST, "rb");
    if(!f) return 0;
    int a, nr, got = 0;
    while(fscanf(f, "%d %d", &a, &nr) == 2) if(a == app && nr > 0) got = nr;
    fclose(f);
    return got;
}

int data_remove_demo(void){
    FILE *f = fopen(DEMO_MANIFEST, "rb");
    if(!f) return 0;
    int app, nr, removed = 0;
    while(fscanf(f, "%d %d", &app, &nr) == 2){
        if(app < 0 || app > 3 || nr <= 0) continue;
        removed += remove_demo_from(app, (uint32_t)nr);
    }
    fclose(f);
    remove(DEMO_MANIFEST);
    return removed;
}

/* ------------------------- iteration ------------------------- */
typedef struct { data_row_cb cb; void *ctx; } It;

static int cbCal(const PdbRec *r, int i, void *ctx){
    (void)i; It *it=ctx; Appt a;
    if(g_catfilter>=0 && (r->attr & 0x0F)!=g_catfilter) return 0;
    if(ApptUnpack(r->data,r->len,&a)) return 0;
    char pri[96];
    if(a.hasTime) snprintf(pri,sizeof pri,"%d/%d %d:%02d  %.72s",a.month,a.day,a.sH,a.sM,a.description);
    else          snprintf(pri,sizeof pri,"%d/%d  %.72s",a.month,a.day,a.description);
    it->cb(r->uniqueID, pri, NULL, it->ctx);
    return 0;
}
void data_datebook(data_row_cb cb, void *ctx){ It it={cb,ctx}; live_read(DB_CAL,cbCal,&it); }

/* one day's appointments (PalmOS Day view). Primary is "HH:MM  desc" (zero-padded
 * so a lexical sort == chronological; untimed events use "--:--" which sorts to
 * the top, like Palm). Honors the active category filter. */
typedef struct { data_row_cb cb; void *ctx; int y,m,d; } DayIt;
static int cbCalDay(const PdbRec *r, int i, void *ctx){
    (void)i; DayIt *it=ctx; Appt a;
    if(g_catfilter>=0 && (r->attr & 0x0F)!=g_catfilter) return 0;
    if(ApptUnpack(r->data,r->len,&a)) return 0;
    if(!appt_occurs_on(&a, it->y, it->m, it->d)) return 0;   /* repeats expanded */
    char pri[96];
    if(a.hasTime) snprintf(pri,sizeof pri,"%02d:%02d  %.80s",a.sH,a.sM,a.description);
    else          snprintf(pri,sizeof pri,"--:--  %.80s",a.description);
    it->cb(r->uniqueID, pri, NULL, it->ctx);
    return 0;
}
void data_cal_day(int y,int m,int d, data_row_cb cb, void *ctx){
    DayIt it={cb,ctx,y,m,d}; live_read(DB_CAL,cbCalDay,&it);
}

/* mark which day-of-month (1..31) has >=1 appointment in month y/m (Month view
 * dots), repeats expanded. marks[0] unused; ignores the category filter
 * (whole-month overview). */
typedef struct { int y,m; uint8_t *marks; } MarkIt;
static int cbCalMark(const PdbRec *r, int i, void *ctx){
    (void)i; MarkIt *mk=ctx; Appt a;
    if(ApptUnpack(r->data,r->len,&a)) return 0;
    if(!a.hasRepeat){
        if(a.year==mk->y && a.month==mk->m && a.day>=1 && a.day<=31) mk->marks[a.day]=1;
        return 0;
    }
    for(int d=1; d<=31; d++) if(!mk->marks[d] && appt_occurs_on(&a, mk->y, mk->m, d)) mk->marks[d]=1;
    return 0;
}
void data_cal_month_marks(int y,int m, uint8_t marks[32]){
    for(int i=0;i<32;i++) marks[i]=0;
    MarkIt mk={y,m,marks}; live_read(DB_CAL,cbCalMark,&mk);
}

static int cbAddr(const PdbRec *r, int i, void *ctx){
    (void)i; It *it=ctx; Addr a;
    if(g_catfilter>=0 && (r->attr & 0x0F)!=g_catfilter) return 0;
    if(AddrUnpack(r->data,r->len,&a)) return 0;
    const char *last=a.fields[F_name], *first=a.fields[F_firstName], *co=a.fields[F_company];
    char pri[96];
    if(last && first) snprintf(pri,sizeof pri,"%.40s, %.40s",last,first);
    else if(last)     snprintf(pri,sizeof pri,"%.80s",last);
    else if(co)       snprintf(pri,sizeof pri,"%.80s",co);
    else              snprintf(pri,sizeof pri,"(no name)");
    it->cb(r->uniqueID, pri, a.fields[F_phone1], it->ctx);
    return 0;
}
void data_address(data_row_cb cb, void *ctx){ It it={cb,ctx}; live_read(DB_ADDR,cbAddr,&it); }

static int cbTodo(const PdbRec *r, int i, void *ctx){
    (void)i; It *it=ctx; Todo t;
    if(g_catfilter>=0 && (r->attr & 0x0F)!=g_catfilter) return 0;
    if(ToDoUnpack(r->data,r->len,&t)) return 0;
    char pri[96];
    snprintf(pri,sizeof pri,"%s%.80s",t.completed?"[x] ":"[ ] ",t.description);
    /* secondary carries structured fields the list UI parses: priority and the
     * due date as YYYYMMDD (0 = no due). Keep "pri %d" leading so older parsers
     * still read the priority. */
    int due = t.hasDue ? (t.dueY*10000 + t.dueM*100 + t.dueD) : 0;
    char sec[32]; snprintf(sec,sizeof sec,"pri %d due %d",t.priority,due);
    it->cb(r->uniqueID, pri, sec, it->ctx);
    return 0;
}
void data_todo(data_row_cb cb, void *ctx){ It it={cb,ctx}; live_read(DB_TODO,cbTodo,&it); }

/* ------------------------- detail (by uid) ------------------------- */
typedef struct { uint32_t uid; char *out; int cap; int found; } Det;

static int detCal(const PdbRec *r, int i, void *ctx){
    (void)i; Det *d=ctx; if(r->uniqueID!=d->uid) return 0;
    Appt a; if(ApptUnpack(r->data,r->len,&a)) return 1;
    int p=0;
    p+=snprintf(d->out+p,d->cap-p,"%.100s\n\n",a.description);
    if(a.hasTime) p+=snprintf(d->out+p,d->cap-p,"%d/%d/%d\n%d:%02d - %d:%02d\n",a.month,a.day,a.year,a.sH,a.sM,a.eH,a.eM);
    else          p+=snprintf(d->out+p,d->cap-p,"%d/%d/%d  (all day)\n",a.month,a.day,a.year);
    if(a.note[0]) snprintf(d->out+p,d->cap-p,"\n%.300s",a.note);
    d->found=1; return 1;
}
static int detAddr(const PdbRec *r, int i, void *ctx){
    (void)i; Det *d=ctx; if(r->uniqueID!=d->uid) return 0;
    Addr a; if(AddrUnpack(r->data,r->len,&a)) return 1;
    const char *first=a.fields[F_firstName], *last=a.fields[F_name];
    int p=0;
    p+=snprintf(d->out+p,d->cap-p,"%.40s %.40s\n",first?first:"",last?last:"");
    if(a.fields[F_company]) p+=snprintf(d->out+p,d->cap-p,"%.60s\n",a.fields[F_company]);
    if(a.fields[F_title])   p+=snprintf(d->out+p,d->cap-p,"%.60s\n",a.fields[F_title]);
    static const char *plabel[] = {"Work","Home","Fax","Other","Email","Main","Pager","Mobile"};
    for(int k=0;k<5;k++){
        const char *ph=a.fields[F_phone1+k]; if(!ph||!ph[0]) continue;
        int lab=a.phoneLabel[k]; if(lab<0||lab>7)lab=3;
        p+=snprintf(d->out+p,d->cap-p,"%s: %.40s\n",plabel[lab],ph);
    }
    if(a.fields[F_address]) p+=snprintf(d->out+p,d->cap-p,"\n%.60s\n",a.fields[F_address]);
    if(a.fields[F_city])    p+=snprintf(d->out+p,d->cap-p,"%.40s %.20s %.16s\n",a.fields[F_city],a.fields[F_state]?a.fields[F_state]:"",a.fields[F_zip]?a.fields[F_zip]:"");
    if(a.fields[F_note])    snprintf(d->out+p,d->cap-p,"\n%.200s",a.fields[F_note]);
    d->found=1; return 1;
}
static int detTodo(const PdbRec *r, int i, void *ctx){
    (void)i; Det *d=ctx; if(r->uniqueID!=d->uid) return 0;
    Todo t; if(ToDoUnpack(r->data,r->len,&t)) return 1;
    int p=0;
    p+=snprintf(d->out+p,d->cap-p,"%.100s\n\n",t.description);
    p+=snprintf(d->out+p,d->cap-p,"Priority: %d\nStatus: %s\n",t.priority,t.completed?"Completed":"Open");
    if(t.hasDue) p+=snprintf(d->out+p,d->cap-p,"Due: %d/%d/%d\n",t.dueM,t.dueD,t.dueY);
    if(t.note[0]) snprintf(d->out+p,d->cap-p,"\n%.300s",t.note);
    d->found=1; return 1;
}

static int getMemo(const PdbRec *r, int i, void *ctx);   /* fwd (also the memo detail) */
int data_detail(int app, uint32_t uid, char *out, int cap){
    if(cap>0) out[0]=0;
    Det d = { uid, out, cap, 0 };
    switch(app){
        case APP_CAL:  live_read(DB_CAL,  detCal,  &d); break;
        case APP_ADDR: live_read(DB_ADDR, detAddr, &d); break;
        case APP_TODO: live_read(DB_TODO, detTodo, &d); break;
        case APP_MEMO: live_read(DB_MEMO, getMemo, &d); break;
    }
    return d.found;
}

/* ------------------------- get one record ------------------------- */
#define REC_ATTR_DIRTY 0x40
typedef struct { uint32_t uid; void *out; int found; } Get;
static int getCal(const PdbRec *r,int i,void *ctx){ (void)i; Get*g=ctx;
    if(r->uniqueID!=g->uid) return 0;
    if(ApptUnpack(r->data,r->len,(Appt*)g->out)==0) g->found=1;
    return 1; }
static int getAddr(const PdbRec *r,int i,void *ctx){ (void)i; Get*g=ctx;
    if(r->uniqueID!=g->uid) return 0;
    if(AddrUnpack(r->data,r->len,(Addr*)g->out)==0) g->found=1;
    return 1; }
static int getTodo(const PdbRec *r,int i,void *ctx){ (void)i; Get*g=ctx;
    if(r->uniqueID!=g->uid) return 0;
    if(ToDoUnpack(r->data,r->len,(Todo*)g->out)==0) g->found=1;
    return 1; }

int data_get_cal(uint32_t uid, Appt *out){ Get g={uid,out,0}; live_read(DB_CAL,getCal,&g); return g.found; }
int data_get_addr(uint32_t uid, Addr *out){ Get g={uid,out,0}; live_read(DB_ADDR,getAddr,&g); return g.found; }
int data_get_todo(uint32_t uid, Todo *out){ Get g={uid,out,0}; live_read(DB_TODO,getTodo,&g); return g.found; }

/* ------------------------- rewrite (replace/append one record) ------------------------- */
typedef struct { uint32_t uid; const uint8_t *nd; int nl; int cat; int done; int tomb; } RW;
static int rwFate(const PdbRec *r, uint8_t *attr, const uint8_t **d, int *len, void *ctx){
    RW *w = ctx;
    if(r->uniqueID != w->uid) return 0;
    w->done = 1;
    if(!w->nd && !w->tomb) return 1;                    /* delete: drop this record */
    if(!w->nd){ *attr |= REC_ATTR_DELETE | REC_ATTR_DIRTY; return 0; }   /* a tombstone */
    *d = w->nd; *len = w->nl;
    if(w->cat >= 0) *attr = (*attr & ~0x0F) | (uint8_t)(w->cat & 0x0F);  /* recategorize */
    *attr |= REC_ATTR_DIRTY;
    return 0;
}
/* a new record's uid: past every existing one, and never inside the demo
 * seed's range (1..n): HotSync holds those back from the push, so a user's
 * record that landed there -- after every sample was deleted, say -- would
 * silently never sync. */
static uint32_t new_uid(const char *path, uint32_t maxuid){
    uint32_t nu = maxuid + 1;
    for(int a = 0; a < 4; a++)
        if(db_path(a) == path || !strcmp(db_path(a), path)){
            uint32_t dn = (uint32_t)data_demo_count(a);
            if(nu <= dn) nu = dn + 1;
        }
    return nu;
}
typedef struct { uint32_t uid; int found; } Look;
static int lookCb(const PdbRec *r, int i, void *c){ (void)i; Look *l = c;
    if(r->uniqueID == l->uid){ l->found = 1; return 1; }
    return 0;
}
/* replace/append/delete one record, preserving the PDB's AppInfo (categories).
 * nd==NULL => delete; cat<0 => keep the record's current category. The
 * database streams through the card (stream_rewrite), whatever its size. */
static int rewrite_ex(const char *path,const char *nm,uint32_t type,uint32_t creator,
                      uint32_t uid,const uint8_t *nd,int nl,int cat,int tomb){
    RW w = { uid, nd, nl, cat, 0, tomb };
    /* the record to append is decided after the walk (was uid found?), so
     * walk once to look, then rewrite: both are reads of the card, no RAM */
    if(nd && uid){
        Look lk = { uid, 0 };
        pdb_read(path, lookCb, &lk);
        if(lk.found) return stream_rewrite(path,nm,type,creator,NULL,0,rwFate,&w,NULL,NULL);
    }
    if(nd){                                   /* new record (uid==0 or not found): append */
        PdbRec add = { .attr = REC_ATTR_DIRTY | (cat>=0 ? (uint8_t)(cat & 0x0F) : 0), .uniqueID = 0,
                       .data = nd, .len = nl };
        return stream_rewrite(path,nm,type,creator,NULL,0,rwFate,&w,&add,new_uid);
    }
    return stream_rewrite(path,nm,type,creator,NULL,0,rwFate,&w,NULL,NULL);
}
static int rewrite(const char *path,const char *nm,uint32_t type,uint32_t creator,
                   uint32_t uid,const uint8_t *nd,int nl,int cat){
    return rewrite_ex(path,nm,type,creator,uid,nd,nl,cat,0);
}

int data_save_cal(uint32_t uid, int cat, const Appt *in){
    uint8_t pk[1024]; int l=ApptPack(pk,sizeof pk,in); if(l<=0) return 0;
    return rewrite(DB_CAL,"DatebookDB",0x44415441,0x64617465,uid,pk,l,cat)>=0;
}
int data_save_addr(uint32_t uid, int cat, const Addr *in){
    uint8_t pk[1024]; int l=AddrPack(pk,sizeof pk,in); if(l<=0) return 0;
    return rewrite(DB_ADDR,"AddressDB",0x44415441,0x61646472,uid,pk,l,cat)>=0;
}
int data_save_todo(uint32_t uid, int cat, const Todo *in){
    uint8_t pk[1024]; int l=ToDoPack(pk,sizeof pk,in); if(l<=0) return 0;
    return rewrite(DB_TODO,"ToDoDB",0x44415441,0x746F646F,uid,pk,l,cat)>=0;
}

/* Does this app's database sync anywhere? Only then is a tombstone worth
 * keeping: Memo never syncs, and an app with no account or no collection has no
 * sync to carry the deletion to -- a tombstone there would sit forever. */
static int app_syncs(int app){
    const Config *c = appcfg();
    if(!c->dav_user[0]) return 0;
    switch(app){
        case APP_CAL:  return c->cal_coll[0]  != 0;
        case APP_TODO: return c->todo_coll[0] != 0;
        case APP_ADDR: return c->card_coll[0] != 0;
    }
    return 0;
}

int data_app_syncs(int app){ return app_syncs(app); }

/* Delete a record. In an app that syncs it becomes a tombstone (see live_read)
 * and the next sync drops it from the device, leaving the server's copy alone
 * (sync.c, HIDDEN_HASH); elsewhere it is removed outright, as it always was. */
int data_delete(int app, uint32_t uid){
    int t = app_syncs(app);
    switch(app){
        case APP_CAL:  return rewrite_ex(DB_CAL, "DatebookDB",0x44415441,0x64617465,uid,NULL,0,-1,t)>=0;
        case APP_ADDR: return rewrite_ex(DB_ADDR,"AddressDB", 0x44415441,0x61646472,uid,NULL,0,-1,t)>=0;
        case APP_TODO: return rewrite_ex(DB_TODO,"ToDoDB",    0x44415441,0x746F646F,uid,NULL,0,-1,t)>=0;
        case APP_MEMO: return rewrite(DB_MEMO,"MemoDB",    0x44415441,0x6D656D6F,uid,NULL,0,-1)>=0;
    }
    return 0;
}

/* ---- memo (records are plain text) ---- */
static int cbMemo(const PdbRec *r, int i, void *ctx){
    (void)i; It *it=ctx;
    if(g_catfilter>=0 && (r->attr & 0x0F)!=g_catfilter) return 0;
    const char *s=(const char *)r->data;
    char pri[96]; int j=0;
    while(s[j] && s[j]!='\n' && j<(int)sizeof pri-1){ pri[j]=s[j]; j++; }
    pri[j]=0;
    if(!pri[0]) snprintf(pri,sizeof pri,"(empty memo)");
    it->cb(r->uniqueID, pri, NULL, it->ctx);
    return 0;
}
void data_memo(data_row_cb cb, void *ctx){ It it={cb,ctx}; live_read(DB_MEMO,cbMemo,&it); }

static int getMemo(const PdbRec *r, int i, void *ctx){
    (void)i; Det *g=ctx; if(r->uniqueID!=g->uid) return 0;
    snprintf(g->out, g->cap, "%.*s", r->len, (const char *)r->data);
    g->found=1; return 1;
}
int data_get_memo(uint32_t uid, char *out, int cap){
    if(cap>0) out[0]=0;
    Det g={uid,out,cap,0}; live_read(DB_MEMO,getMemo,&g); return g.found;
}
int data_save_memo(uint32_t uid, int cat, const char *text){
    int l=(int)strlen(text)+1;
    return rewrite(DB_MEMO,"MemoDB",0x44415441,0x6D656D6F,uid,(const uint8_t *)text,l,cat)>=0;
}

/* flip a to-do's completed flag and save it (keeps its category). 1 on success. */
int data_toggle_todo(uint32_t uid){
    Todo t; if(!data_get_todo(uid,&t)) return 0;
    t.completed = !t.completed;
    return data_save_todo(uid, -1, &t);
}

/* the category index of a record (attr nibble), or -1 if not found */
typedef struct { uint32_t uid; int cat; } CatOf;
static int catCb(const PdbRec *r,int i,void *ctx){ (void)i; CatOf*c=ctx;
    if(r->uniqueID==c->uid){ c->cat=r->attr & 0x0F; return 1; } return 0; }
int data_record_category(int app, uint32_t uid){
    CatOf c={uid,-1}; live_read(db_path(app),catCb,&c); return c.cat;
}
