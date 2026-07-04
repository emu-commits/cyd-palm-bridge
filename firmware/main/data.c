/* data.c -- read PDB records from the SD card and hand them to the UI as display
 * strings. Also seeds demo PDBs so the views have content before a HotSync (U7).
 * Uses the shared codec (bridge component) directly; one record in RAM at a time.
 */
#include "data.h"
#include "palm.h"
#include "appinfo.h"
#include <stdio.h>
#include <string.h>

#define DB_CAL  "/sdcard/DatebookDB.pdb"
#define DB_ADDR "/sdcard/AddressDB.pdb"
#define DB_TODO "/sdcard/ToDoDB.pdb"
#define DB_MEMO "/sdcard/MemoDB.pdb"

static const char *db_path(int app){
    return app==APP_CAL?DB_CAL : app==APP_ADDR?DB_ADDR : app==APP_TODO?DB_TODO : DB_MEMO;
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
 * Enough rows to overflow the list (scroll test). This overwrites each boot for
 * now; U7 HotSync replaces it with real synced data. */
#define SEEDMAX 32
static void seed_datebook(void){
    static uint8_t arena[SEEDMAX*96]; PdbRec r[SEEDMAX]; int nr=0, used=0;
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
    pdb_write_ai(DB_CAL,"DatebookDB",0x44415441,0x64617465,ai,al,r,nr);
}

static void seed_address(void){
    static uint8_t arena[SEEDMAX*96]; PdbRec r[SEEDMAX]; int nr=0, used=0;
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
    pdb_write_ai(DB_ADDR,"AddressDB",0x44415441,0x61646472,ai,al,r,nr);
}

static void seed_todo(void){
    static uint8_t arena[SEEDMAX*96]; PdbRec r[SEEDMAX]; int nr=0, used=0;
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
    pdb_write_ai(DB_TODO,"ToDoDB",0x44415441,0x746F646F,ai,al,r,nr);
}

static void seed_memo(void){
    static uint8_t arena[SEEDMAX*256]; PdbRec r[SEEDMAX]; int nr=0, used=0;
    uint8_t ai[APPINFO_SIZE]; int al=build_appinfo(ai);
    const char *memos[] = {
        "Shopping list\nMilk\nEggs\nBread\nCoffee",
        "CYD project ideas\n- Palm-style UI\n- iCloud sync\n- battery + case",
        "Meeting notes 8/3\n- discuss sync\n- test on device\n- ship it",
        "Wifi password is on the fridge" };
    int n=(int)(sizeof(memos)/sizeof(memos[0]));
    for(int i=0;i<n && nr<SEEDMAX;i++){
        int len=(int)strlen(memos[i])+1;
        if(used+len>(int)sizeof arena) break;
        memcpy(arena+used, memos[i], len);
        r[nr]=(PdbRec){.attr=(uint8_t)(i%3),.uniqueID=(uint32_t)(i+1),.data=arena+used,.len=len};
        used+=len; nr++;
    }
    pdb_write_ai(DB_MEMO,"MemoDB",0x44415441,0x6D656D6F,ai,al,r,nr);
}

void data_seed_if_empty(void){
    /* only seed a DB that doesn't exist yet, so edits + synced data persist */
    if(!file_exists(DB_CAL))  seed_datebook();
    if(!file_exists(DB_ADDR)) seed_address();
    if(!file_exists(DB_TODO)) seed_todo();
    if(!file_exists(DB_MEMO)) seed_memo();
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
    char sec[16]; snprintf(sec,sizeof sec,"pri %d",t.priority);
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
#define RW_ARENA (12*1024)
#define RW_MAX   64
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
    static uint8_t arena[RW_ARENA]; static PdbRec recs[RW_MAX];
    uint8_t ai[512]; int al=pdb_read_appinfo(path,ai,sizeof ai); if(al<0)al=0;
    RW w={uid,nd,nl,cat,arena,0,recs,0,0};
    pdb_read(path,rwCb,&w);
    if(!w.done && nd){                /* new record (uid==0 or not found): append */
        uint32_t nu=1; for(int i=0;i<w.nr;i++) if(recs[i].uniqueID>=nu) nu=recs[i].uniqueID+1;
        if(w.used+nl<=RW_ARENA && w.nr<RW_MAX){
            memcpy(arena+w.used,nd,nl);
            uint8_t na=REC_ATTR_DIRTY | (cat>=0 ? (uint8_t)(cat & 0x0F) : 0);
            recs[w.nr]=(PdbRec){.attr=na,.uniqueID=nu,.data=arena+w.used,.len=nl};
            w.used+=nl; w.nr++;
        }
    }
    return pdb_write_ai(path,nm,type,creator, al?ai:NULL, al, recs, w.nr);
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

/* the category index of a record (attr nibble), or -1 if not found */
typedef struct { uint32_t uid; int cat; } CatOf;
static int catCb(const PdbRec *r,int i,void *ctx){ (void)i; CatOf*c=ctx;
    if(r->uniqueID==c->uid){ c->cat=r->attr & 0x0F; return 1; } return 0; }
int data_record_category(int app, uint32_t uid){
    CatOf c={uid,-1}; pdb_read(db_path(app),catCb,&c); return c.cat;
}
