/* data.c -- read PDB records from the SD card and hand them to the UI as display
 * strings. Also seeds demo PDBs so the views have content before a HotSync (U7).
 * Uses the shared codec (bridge component) directly; one record in RAM at a time.
 */
#include "data.h"
#include "palm.h"
#include "appinfo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_CAL  "/sdcard/DatebookDB.pdb"
#define DB_ADDR "/sdcard/AddressDB.pdb"
#define DB_TODO "/sdcard/ToDoDB.pdb"
#define DB_MEMO "/sdcard/MemoDB.pdb"
/* I2: records the demo seed so "Remove demo data" can delete exactly what was
 * seeded (per-app record count; seeds always take uniqueIDs 1..nr, and new/synced
 * records get max+1, so 1..nr uniquely identifies the demo rows). */
#define DEMO_MANIFEST "/sdcard/.demoseed"

static const char *db_path(int app){
    return app==APP_CAL?DB_CAL : app==APP_ADDR?DB_ADDR : app==APP_TODO?DB_TODO : DB_MEMO;
}
const char *data_db_path(int app){ return db_path(app); }
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
 * Enough rows to overflow the list (scroll test). This overwrites each boot for
 * now; U7 HotSync replaces it with real synced data. */
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

/* Migrate PDBs seeded by an older build that had no AppInfo (so the category
 * table was empty and the picker only offered "All"). Reads the records back,
 * rewrites the PDB with the demo category table, and preserves the records. */
#define RW_ARENA (12*1024)
#define RW_MAX   64
static uint8_t g_arena[RW_ARENA];
static PdbRec  g_recs[RW_MAX];
typedef struct { int used; int nr; } Collect;
static int collectCb(const PdbRec *r, int i, void *ctx){ (void)i; Collect *c=ctx;
    if(c->used + r->len > RW_ARENA || c->nr >= RW_MAX) return 1;
    memcpy(g_arena+c->used, r->data, r->len);
    g_recs[c->nr]=(PdbRec){.attr=r->attr,.uniqueID=r->uniqueID,.data=g_arena+c->used,.len=r->len};
    c->used+=r->len; c->nr++; return 0;
}
static void ensure_appinfo(const char *path,const char *nm,uint32_t type,uint32_t creator){
    if(!file_exists(path)) return;
    uint8_t ai[512]; int al=pdb_read_appinfo(path,ai,sizeof ai);
    CatTable t;
    if(al>0 && appinfo_parse(ai,al,&t)==0 && t.name[0][0]) return;   /* already has categories */
    Collect c={0,0}; pdb_read(path,collectCb,&c);
    uint8_t nai[APPINFO_SIZE]; int nal=build_appinfo(nai);
    pdb_write_ai(path,nm,type,creator,nai,nal,g_recs,c.nr);
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

/* C4: write a new category table into the app's PDB AppInfo, preserving every
 * record (the low-nibble category on each record is untouched). Mirrors
 * ensure_appinfo's rewrite; 1 on success, 0 on failure. */
int data_set_categories(int app, const CatTable *t){
    const char *path = db_path(app);
    if(!file_exists(path)) return 0;
    uint8_t nai[APPINFO_SIZE]; int nal = appinfo_build(nai, sizeof nai, t);
    if(nal < 0) return 0;
    Collect c={0,0}; pdb_read(path,collectCb,&c);
    const char *nm; uint32_t type, creator; db_ident(app,&nm,&type,&creator);
    return pdb_write_ai(path,nm,type,creator,nai,nal,g_recs,c.nr) >= 0;
}

void data_seed_if_empty(void){
    /* only seed a DB that doesn't exist yet, so edits + synced data persist */
    int seeded[4] = { -1, -1, -1, -1 };
    if(!file_exists(DB_CAL))  seeded[APP_CAL]  = seed_datebook();
    if(!file_exists(DB_ADDR)) seeded[APP_ADDR] = seed_address();
    if(!file_exists(DB_TODO)) seeded[APP_TODO] = seed_todo();
    if(!file_exists(DB_MEMO)) seeded[APP_MEMO] = seed_memo();
    /* I2: if we seeded anything this boot, record it so the demo rows can be
     * removed as a set before the first HotSync (don't push fake data to iCloud) */
    int any = 0;
    for(int i=0;i<4;i++) if(seeded[i] > 0) any = 1;
    if(any){
        FILE *f = fopen(DEMO_MANIFEST, "wb");
        if(f){
            for(int i=0;i<4;i++) if(seeded[i] > 0) fprintf(f, "%d %d\n", i, seeded[i]);
            fclose(f);
        }
    }
    /* backfill categories into PDBs from older builds that lacked AppInfo */
    ensure_appinfo(DB_CAL, "DatebookDB",0x44415441,0x64617465);
    ensure_appinfo(DB_ADDR,"AddressDB", 0x44415441,0x61646472);
    ensure_appinfo(DB_TODO,"ToDoDB",    0x44415441,0x746F646F);
    ensure_appinfo(DB_MEMO,"MemoDB",    0x44415441,0x6D656D6F);
}

/* I2: is the (still-untouched) demo seed present? Drives the "Remove demo data"
 * menu item. The manifest is deleted once the demo rows are removed. */
int data_demo_present(void){ return file_exists(DEMO_MANIFEST); }

/* one-pass filter: keep every record whose uniqueID is outside 1..maxuid (i.e.
 * drop the demo rows), counting how many were dropped. */
typedef struct { uint32_t maxuid; uint8_t *arena; int used; PdbRec *recs; int nr; int removed; } DemoRW;
static int demoRwCb(const PdbRec *r, int i, void *ctx){ (void)i; DemoRW *w=ctx;
    if(r->uniqueID >= 1 && r->uniqueID <= w->maxuid){ w->removed++; return 0; }  /* drop */
    if(w->used + r->len > RW_ARENA || w->nr >= RW_MAX) return 1;
    memcpy(w->arena + w->used, r->data, r->len);
    w->recs[w->nr] = (PdbRec){.attr=r->attr,.uniqueID=r->uniqueID,.data=w->arena+w->used,.len=r->len};
    w->used += r->len; w->nr++; return 0;
}
/* rewrite one app's PDB once, dropping demo rows (uid 1..nr); returns count removed. */
static int remove_demo_from(int app, uint32_t nr){
    const char *path = db_path(app);
    if(!file_exists(path)) return 0;
    uint8_t ai[512]; int al = pdb_read_appinfo(path, ai, sizeof ai); if(al<0) al=0;
    DemoRW w = { nr, g_arena, 0, g_recs, 0, 0 };
    pdb_read(path, demoRwCb, &w);
    const char *nm; uint32_t type, creator;
    switch(app){
        case APP_CAL:  nm="DatebookDB"; type=0x44415441; creator=0x64617465; break;
        case APP_ADDR: nm="AddressDB";  type=0x44415441; creator=0x61646472; break;
        case APP_TODO: nm="ToDoDB";     type=0x44415441; creator=0x746F646F; break;
        default:       nm="MemoDB";     type=0x44415441; creator=0x6D656D6F; break;
    }
    pdb_write_ai(path, nm, type, creator, al?ai:NULL, al, g_recs, w.nr);
    return w.removed;
}

/* I2: delete exactly the demo-seeded records (uniqueIDs 1..nr per app from the
 * manifest -- ONE rewrite per app, not per record), then drop the manifest.
 * Returns the number of records removed. User-added / synced records
 * (uniqueID > nr) are never touched. */
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
void data_datebook(data_row_cb cb, void *ctx){ It it={cb,ctx}; pdb_read(DB_CAL,cbCal,&it); }

/* one day's appointments (PalmOS Day view). Primary is "HH:MM  desc" (zero-padded
 * so a lexical sort == chronological; untimed events use "--:--" which sorts to
 * the top, like Palm). Honors the active category filter. */
typedef struct { data_row_cb cb; void *ctx; int y,m,d; } DayIt;
static int cbCalDay(const PdbRec *r, int i, void *ctx){
    (void)i; DayIt *it=ctx; Appt a;
    if(g_catfilter>=0 && (r->attr & 0x0F)!=g_catfilter) return 0;
    if(ApptUnpack(r->data,r->len,&a)) return 0;
    if(a.year!=it->y || a.month!=it->m || a.day!=it->d) return 0;
    char pri[96];
    if(a.hasTime) snprintf(pri,sizeof pri,"%02d:%02d  %.80s",a.sH,a.sM,a.description);
    else          snprintf(pri,sizeof pri,"--:--  %.80s",a.description);
    it->cb(r->uniqueID, pri, NULL, it->ctx);
    return 0;
}
void data_cal_day(int y,int m,int d, data_row_cb cb, void *ctx){
    DayIt it={cb,ctx,y,m,d}; pdb_read(DB_CAL,cbCalDay,&it);
}

/* mark which day-of-month (1..31) has >=1 appointment in month y/m (Month view
 * dots). marks[0] unused; ignores the category filter (whole-month overview). */
typedef struct { int y,m; uint8_t *marks; } MarkIt;
static int cbCalMark(const PdbRec *r, int i, void *ctx){
    (void)i; MarkIt *mk=ctx; Appt a;
    if(ApptUnpack(r->data,r->len,&a)) return 0;
    if(a.year==mk->y && a.month==mk->m && a.day>=1 && a.day<=31) mk->marks[a.day]=1;
    return 0;
}
void data_cal_month_marks(int y,int m, uint8_t marks[32]){
    for(int i=0;i<32;i++) marks[i]=0;
    MarkIt mk={y,m,marks}; pdb_read(DB_CAL,cbCalMark,&mk);
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
void data_address(data_row_cb cb, void *ctx){ It it={cb,ctx}; pdb_read(DB_ADDR,cbAddr,&it); }

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
void data_todo(data_row_cb cb, void *ctx){ It it={cb,ctx}; pdb_read(DB_TODO,cbTodo,&it); }

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
        case APP_CAL:  pdb_read(DB_CAL,  detCal,  &d); break;
        case APP_ADDR: pdb_read(DB_ADDR, detAddr, &d); break;
        case APP_TODO: pdb_read(DB_TODO, detTodo, &d); break;
        case APP_MEMO: pdb_read(DB_MEMO, getMemo, &d); break;
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

int data_get_cal(uint32_t uid, Appt *out){ Get g={uid,out,0}; pdb_read(DB_CAL,getCal,&g); return g.found; }
int data_get_addr(uint32_t uid, Addr *out){ Get g={uid,out,0}; pdb_read(DB_ADDR,getAddr,&g); return g.found; }
int data_get_todo(uint32_t uid, Todo *out){ Get g={uid,out,0}; pdb_read(DB_TODO,getTodo,&g); return g.found; }

/* ------------------------- rewrite (replace/append one record) ------------------------- */
typedef struct { uint32_t uid; const uint8_t *nd; int nl; int cat; uint8_t *arena; int used; PdbRec *recs; int nr; int done; } RW;
static int rwCb(const PdbRec *r,int i,void *ctx){ (void)i; RW*w=ctx;
    const uint8_t *src=r->data; int len=r->len; uint8_t attr=r->attr;
    if(r->uniqueID==w->uid){
        if(!w->nd){ w->done=1; return 0; }             /* delete: drop this record */
        src=w->nd; len=w->nl;
        if(w->cat>=0) attr=(attr & ~0x0F) | (uint8_t)(w->cat & 0x0F);   /* recategorize */
        attr|=REC_ATTR_DIRTY; w->done=1;
    }
    if(w->used+len>RW_ARENA || w->nr>=RW_MAX) return 1;
    memcpy(w->arena+w->used, src, len);
    w->recs[w->nr]=(PdbRec){.attr=attr,.uniqueID=r->uniqueID,.data=w->arena+w->used,.len=len};
    w->used+=len; w->nr++; return 0;
}
/* replace/append/delete one record, preserving the PDB's AppInfo (categories).
 * nd==NULL => delete; cat<0 => keep the record's current category. */
static int rewrite(const char *path,const char *nm,uint32_t type,uint32_t creator,
                   uint32_t uid,const uint8_t *nd,int nl,int cat){
    uint8_t ai[512]; int al=pdb_read_appinfo(path,ai,sizeof ai); if(al<0)al=0;
    RW w={uid,nd,nl,cat,g_arena,0,g_recs,0,0};
    pdb_read(path,rwCb,&w);
    if(!w.done && nd){                /* new record (uid==0 or not found): append */
        uint32_t nu=1; for(int i=0;i<w.nr;i++) if(g_recs[i].uniqueID>=nu) nu=g_recs[i].uniqueID+1;
        if(w.used+nl<=RW_ARENA && w.nr<RW_MAX){
            memcpy(g_arena+w.used,nd,nl);
            uint8_t na=REC_ATTR_DIRTY | (cat>=0 ? (uint8_t)(cat & 0x0F) : 0);
            g_recs[w.nr]=(PdbRec){.attr=na,.uniqueID=nu,.data=g_arena+w.used,.len=nl};
            w.used+=nl; w.nr++;
        }
    }
    return pdb_write_ai(path,nm,type,creator, al?ai:NULL, al, g_recs, w.nr);
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

/* remove a record (rewrite the PDB without it). the map still lists it, so the
 * next sync propagates the delete to the server. */
int data_delete(int app, uint32_t uid){
    switch(app){
        case APP_CAL:  return rewrite(DB_CAL, "DatebookDB",0x44415441,0x64617465,uid,NULL,0,-1)>=0;
        case APP_ADDR: return rewrite(DB_ADDR,"AddressDB", 0x44415441,0x61646472,uid,NULL,0,-1)>=0;
        case APP_TODO: return rewrite(DB_TODO,"ToDoDB",    0x44415441,0x746F646F,uid,NULL,0,-1)>=0;
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
void data_memo(data_row_cb cb, void *ctx){ It it={cb,ctx}; pdb_read(DB_MEMO,cbMemo,&it); }

static int getMemo(const PdbRec *r, int i, void *ctx){
    (void)i; Det *g=ctx; if(r->uniqueID!=g->uid) return 0;
    snprintf(g->out, g->cap, "%.*s", r->len, (const char *)r->data);
    g->found=1; return 1;
}
int data_get_memo(uint32_t uid, char *out, int cap){
    if(cap>0) out[0]=0;
    Det g={uid,out,cap,0}; pdb_read(DB_MEMO,getMemo,&g); return g.found;
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
    CatOf c={uid,-1}; pdb_read(db_path(app),catCb,&c); return c.cat;
}
