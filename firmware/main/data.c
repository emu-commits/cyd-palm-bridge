/* data.c -- the Memos database on the SD card, handed to the UI as display
 * strings. Uses the shared PDB codec (bridge component); one record in RAM at
 * a time.
 *
 * Memos is the only data app now. Date Book, Address and To Do were archived
 * on 2026-11-07 with the iCloud sync (git tag archive/pim-caldav): their
 * databases stay on any card that has them, untouched, and nothing here reads
 * them. There is no demo data: a card without a MemoDB.pdb gets an empty one
 * with the three standard categories.
 */
#include "data.h"
#include "palm.h"
#include "appinfo.h"
#include "safefile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_MEMO "/sdcard/MemoDB.pdb"
#define MEMO_NAME    "MemoDB"
#define MEMO_TYPE    0x44415441u   /* 'DATA' */
#define MEMO_CREATOR 0x6D656D6Fu   /* 'memo' */

const char *data_db_path(int app){ (void)app; return DB_MEMO; }

static int file_exists(const char *path){
    FILE *f = fopen(path, "rb");
    if(f){ fclose(f); return 1; }
    return 0;
}

/* the category table a new database starts with (Unfiled/Business/Personal) */
static int build_appinfo(uint8_t *ai){
    CatTable t; memset(&t,0,sizeof t);
    strcpy(t.name[0],"Unfiled"); strcpy(t.name[1],"Business"); strcpy(t.name[2],"Personal");
    return appinfo_build(ai, APPINFO_SIZE, &t);
}

/* current category filter for the list: -1 = All, else a category index */
static int g_catfilter = -1;
void data_set_category(int cat){ g_catfilter = cat; }
int  data_get_category(void){ return g_catfilter; }

int data_get_categories(int app, CatTable *t){
    uint8_t ai[512]; int al = pdb_read_appinfo(data_db_path(app), ai, sizeof ai);
    if(al<=0) return 0;
    return appinfo_parse(ai, al, t)==0;
}

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

/* A database from an older build with no AppInfo (so the picker offered only
 * "All"): rewritten with the standard category table, every record kept. */
static void ensure_appinfo(void){
    if(!file_exists(DB_MEMO)) return;
    uint8_t ai[512]; int al=pdb_read_appinfo(DB_MEMO,ai,sizeof ai);
    CatTable t;
    if(al>0 && appinfo_parse(ai,al,&t)==0 && t.name[0][0]) return;   /* already has categories */
    uint8_t nai[APPINFO_SIZE]; int nal=build_appinfo(nai);
    stream_rewrite(DB_MEMO,MEMO_NAME,MEMO_TYPE,MEMO_CREATOR,nai,nal,NULL,NULL,NULL,NULL);
}

/* write a new category table into the database's AppInfo, preserving every
 * record (the low-nibble category on each record is untouched). 1 on success. */
int data_set_categories(int app, const CatTable *t){
    (void)app;
    if(!file_exists(DB_MEMO)) return 0;
    uint8_t nai[APPINFO_SIZE]; int nal = appinfo_build(nai, sizeof nai, t);
    if(nal < 0) return 0;
    return stream_rewrite(DB_MEMO,MEMO_NAME,MEMO_TYPE,MEMO_CREATOR,nai,nal,NULL,NULL,NULL,NULL) >= 0;
}

void data_init(void){
    /* Finish any swap a crash interrupted BEFORE asking whether the database
     * exists: the one state a crash mid-swap can leave is "no MemoDB.pdb, a
     * complete MemoDB.pdb.tmp", and "does not exist" would make a new, empty
     * one over the user's memos. */
    sf_recover(DB_MEMO);
    if(!file_exists(DB_MEMO)){
        uint8_t ai[APPINFO_SIZE]; int al = build_appinfo(ai);
        pdb_write_ai(DB_MEMO, MEMO_NAME, MEMO_TYPE, MEMO_CREATOR, ai, al, NULL, 0);
    }
    ensure_appinfo();
}

/* ------------------------- one memo, by uid ------------------------- */
typedef struct { uint32_t uid; char *out; int cap; int found; } Det;
typedef struct { data_row_cb cb; void *ctx; } It;
#define REC_ATTR_DIRTY 0x40

/* ------------------------- rewrite (replace/append/drop one record) ------------------------- */
typedef struct { uint32_t uid; const uint8_t *nd; int nl; int cat; int done; } RW;
static int rwFate(const PdbRec *r, uint8_t *attr, const uint8_t **d, int *len, void *ctx){
    RW *w = ctx;
    if(r->uniqueID != w->uid) return 0;
    w->done = 1;
    if(!w->nd) return 1;                                /* delete: drop this record */
    *d = w->nd; *len = w->nl;
    if(w->cat >= 0) *attr = (*attr & ~0x0F) | (uint8_t)(w->cat & 0x0F);  /* recategorize */
    *attr |= REC_ATTR_DIRTY;
    return 0;
}
static uint32_t s_last_new_uid;
static uint32_t new_uid(const char *path, uint32_t maxuid){
    (void)path;
    s_last_new_uid = maxuid + 1;
    return s_last_new_uid;
}
uint32_t data_last_new_uid(void){ return s_last_new_uid; }
typedef struct { uint32_t uid; int found; } Look;
static int lookCb(const PdbRec *r, int i, void *c){ (void)i; Look *l = c;
    if(r->uniqueID == l->uid){ l->found = 1; return 1; }
    return 0;
}
/* replace/append/delete one record, preserving the AppInfo (categories).
 * nd==NULL => delete; cat<0 => keep the record's current category. The
 * database streams through the card (stream_rewrite), whatever its size. */
static int rewrite(uint32_t uid, const uint8_t *nd, int nl, int cat){
    RW w = { uid, nd, nl, cat, 0 };
    if(!file_exists(DB_MEMO)) data_init();
    /* the record to append is decided after the walk (was uid found?), so
     * walk once to look, then rewrite: both are reads of the card, no RAM */
    if(nd && uid){
        Look lk = { uid, 0 };
        pdb_read(DB_MEMO, lookCb, &lk);
        if(lk.found) return stream_rewrite(DB_MEMO,MEMO_NAME,MEMO_TYPE,MEMO_CREATOR,NULL,0,rwFate,&w,NULL,NULL);
    }
    if(nd){                                   /* new record (uid==0 or not found): append */
        PdbRec add = { .attr = REC_ATTR_DIRTY | (cat>=0 ? (uint8_t)(cat & 0x0F) : 0), .uniqueID = 0,
                       .data = nd, .len = nl };
        return stream_rewrite(DB_MEMO,MEMO_NAME,MEMO_TYPE,MEMO_CREATOR,NULL,0,rwFate,&w,&add,new_uid);
    }
    return stream_rewrite(DB_MEMO,MEMO_NAME,MEMO_TYPE,MEMO_CREATOR,NULL,0,rwFate,&w,NULL,NULL);
}

int data_delete(int app, uint32_t uid){ (void)app; return rewrite(uid,NULL,0,-1)>=0; }

/* ---- memos (records are plain text) ---- */
/* Records an older build marked deleted for a sync are skipped like gone. */
static int cbMemo(const PdbRec *r, int i, void *ctx){
    (void)i; It *it=ctx;
    if(r->attr & REC_ATTR_DELETE) return 0;
    if(g_catfilter>=0 && (r->attr & 0x0F)!=g_catfilter) return 0;
    const char *s=(const char *)r->data;
    char pri[96]; int j=0;
    while(j < r->len && s[j] && s[j]!='\n' && j<(int)sizeof pri-1){ pri[j]=s[j]; j++; }
    pri[j]=0;
    if(!pri[0]) snprintf(pri,sizeof pri,"(empty memo)");
    it->cb(r->uniqueID, pri, NULL, it->ctx);
    return 0;
}
void data_memo(data_row_cb cb, void *ctx){ It it={cb,ctx}; pdb_read(DB_MEMO,cbMemo,&it); }

static int getMemo(const PdbRec *r, int i, void *ctx){
    (void)i; Det *g=ctx;
    if(r->uniqueID!=g->uid || (r->attr & REC_ATTR_DELETE)) return 0;
    snprintf(g->out, g->cap, "%.*s", r->len, (const char *)r->data);
    g->found=1; return 1;
}
int data_get_memo(uint32_t uid, char *out, int cap){
    if(cap>0) out[0]=0;
    Det g={uid,out,cap,0}; pdb_read(DB_MEMO,getMemo,&g); return g.found;
}
int data_detail(int app, uint32_t uid, char *out, int cap){ (void)app; return data_get_memo(uid, out, cap); }
int data_save_memo(uint32_t uid, int cat, const char *text){
    int l=(int)strlen(text)+1;
    return rewrite(uid,(const uint8_t *)text,l,cat)>=0;
}

/* the category index of a record (attr nibble), or -1 if not found */
typedef struct { uint32_t uid; int cat; } CatOf;
static int catCb(const PdbRec *r,int i,void *ctx){ (void)i; CatOf*c=ctx;
    if(r->uniqueID==c->uid && !(r->attr & REC_ATTR_DELETE)){ c->cat=r->attr & 0x0F; return 1; } return 0; }
int data_record_category(int app, uint32_t uid){
    (void)app;
    CatOf c={uid,-1}; pdb_read(DB_MEMO,catCb,&c); return c.cat;
}
