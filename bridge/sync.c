/* sync.c -- push/pull primitives + incremental conflict-aware two-way sync.
 *
 * The incremental engine (sync_collection) is the finished module. Change
 * detection:
 *   - LOCAL change  = record's canonical body hash differs from the hash
 *                     stored in the map at last sync (or the Palm delete bit).
 *   - SERVER change = object's ETag differs from the map's stored ETag (or the
 *                     object vanished / appeared).
 * Reconciliation runs the full (local-state x server-state) matrix, applies a
 * conflict policy when both sides changed, does the DAV ops, and writes both
 * the merged PDB and the refreshed map.
 *
 * Streaming, not buffering (the no-PSRAM device constraint): the merged output
 * PDB is written through a PdbW that spills record bytes to a temp file, and
 * local record bytes are read from the source PDB on demand (pdb_read_one), so
 * neither a full input nor a full output database is ever resident. The only
 * per-record RAM is a compact index (uid/attr/hash/href/etag). See MAXR below.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "palm.h"
#include "appinfo.h"
#include "sync.h"
#include "dav_xml.h"   /* dav_parse_multiget_stream: the batched fetch */

/* The `[sync]` lines that fire on every HEALTHY sync (per-collection read +
 * push/pull summary, and the per-relocation trace) are gated behind SYNC_DEBUG
 * so a release build (host CLI or on-device UART) is quiet. Genuine errors,
 * OOM, dropped-record warnings, and failure notices below stay unconditional.
 * Build the host with -DSYNC_DEBUG (or #define it on device) to restore them. */
#ifdef SYNC_DEBUG
#define SYNC_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define SYNC_LOG(...) ((void)0)
#endif

/* Working-set sizing. NOTE: as of the streaming reconcile (sync_collection /
 * sync_one below), MAXR NO LONGER caps a synced collection -- reconciliation is a
 * disk-backed merge-join whose resident cost during a DAV call is O(1). MAXR now
 * bounds ONLY the legacy full-sync primitives sync_push/sync_pull (used by the
 * bridge_cli one-shot commands, not by HotSync). SYNC_DEVICE_SIZES still forces
 * device sizing on a host build so tests/bigsync.c can prove the streaming engine
 * scales past MAXR records on the desktop. */
#if defined(ESP_PLATFORM) || defined(SYNC_DEVICE_SIZES)
  #define MAXR       24
  #define ARENA_CAP  (8*1024)          /* sync_pull only */
  #define NAMEL_MAX  96
#else
  #define MAXR       256
  #define ARENA_CAP  (MAXR*PALM_REC_MAX)
  #define NAMEL_MAX  256
#endif
#ifdef ESP_PLATFORM
  #define STATE_DIR  "/sdcard/state"   /* device cwd is "/"; temps live on SD */
#else
  #define STATE_DIR  "state"
#endif
#define BODY_TMP STATE_DIR "/.body"    /* PUT body staged here before upload */
#define OUT_TMP  STATE_DIR "/.pdbout"  /* streamed output record bytes         */

/* Sync scratch. Kept OFF the stack (the sync runs single-threaded and never
 * holds two bodies at once; on the device an 8 KB stack frame per record --
 * loadRec runs for every record -- overflowed the sync task stack), but
 * heap-allocated for the sync's LIFETIME rather than resident in BSS:
 * scratch_alloc() acquires them at each public sync entry and sync_free_scratch()
 * releases them, so ~20 KB (BODY_CAP + PALM_REC_MAX + OBJ_FETCH_CAP) returns to
 * the interactive UI between syncs on the no-PSRAM device: a working set
 * that only a sync needs is never resident. */
#define BODY_CAP 8192
static char   *g_body;    /* emit scratch: one object body at a time (BODY_CAP) */
static uint8_t *g_lrec;   /* one lazily-read local record (PALM_REC_MAX)        */
/* defined after g_objbuf (needs its cap); used by the entry points above it. */
static int scratch_alloc(void);
/* defined with the pull-only switch further down; used by pushRec above it. */
static int put_guard(const DavCtx*d,const char*coll,const char*name,const char*ctype,
                     const char*bodyfile,const char*ifmatch,char*etag,int etagcap,int*status);
static int del_guard(const DavCtx*d,const char*coll,const char*name,const char*ifmatch);

/* ---- kind helpers ---- */
static const char* kindExt(int k){ return k==KIND_CARD?"vcf":"ics"; }
static const char* kindCType(int k){ return k==KIND_CARD?"text/vcard; charset=utf-8":"text/calendar; charset=utf-8"; }
static int kindWrite(int k,const char*path,const uint8_t*ai,int ailen,const PdbRec*recs,int nrec){
    if(k==KIND_CAL)  return pdb_write_ai(path,"DatebookDB",0x44415441,0x64617465,ai,ailen,recs,nrec);
    if(k==KIND_TODO) return pdb_write_ai(path,"ToDoDB",   0x44415441,0x746F646F,ai,ailen,recs,nrec);
    return                pdb_write_ai(path,"AddressDB", 0x44415441,0x61646472,ai,ailen,recs,nrec);
}
/* commit a streamed writer to the on-disk PDB with the right name/type/creator. */
static int kindCommit(PdbW*w,int k,const char*path,const uint8_t*ai,int ailen){
    if(k==KIND_CAL)  return pdbw_commit(w,path,"DatebookDB",0x44415441,0x64617465,ai,ailen);
    if(k==KIND_TODO) return pdbw_commit(w,path,"ToDoDB",   0x44415441,0x746F646F,ai,ailen);
    return                pdbw_commit(w,path,"AddressDB", 0x44415441,0x61646472,ai,ailen);
}

/* ======================= full-sync primitives ========================== */
typedef struct { const DavCtx*d; const char*coll; int kind; FILE*map; int n; } PushCtx;

/* pull the UID: property value out of a serialized iCal/vCard object. 0/-1.
 * matches "UID:" only at the start of a line (the object always starts with
 * BEGIN:, so the leading byte can never be a false hit). */
static int objuid_of(const char*obj,char*out,int cap){
    const char*p=obj;
    while((p=strstr(p,"UID:"))){
        if(p==obj || p[-1]=='\n'){
            p+=4; int j=0;
            while(*p && *p!='\r' && *p!='\n' && j<cap-1) out[j++]=*p++;
            out[j]=0; return j>0?0:-1;
        }
        p+=4;
    }
    return -1;
}
/* rewrite the (single) UID: line of a serialized object to carry `uid`. The
 * emitters synthesize UID:palm-<n>@cyd; a record pulled from another client
 * must be pushed back with its ORIGINAL UID (CalDAV/CardDAV treat a resource's
 * UID as immutable), so foreign edits don't 412 or duplicate. No-op if the new
 * uid is NULL/empty or won't fit. */
static void setUidLine(char*buf,int cap,const char*uid){
    if(!uid||!uid[0]) return;
    char*p=buf;
    for(;;){ p=strstr(p,"UID:"); if(!p) return; if(p==buf||p[-1]=='\n') break; p+=4; }
    char*val=p+4; char*eol=val; while(*eol && *eol!='\r' && *eol!='\n') eol++;
    int newlen=(int)strlen(uid), tail=(int)strlen(eol);
    if((int)(val-buf)+newlen+tail+1>cap) return;   /* won't fit; leave as-is */
    memmove(val+newlen,eol,tail+1);                /* shift tail incl NUL */
    memcpy(val,uid,newlen);
}

/* serialize a Palm record to its iCal/vCard object. uidOverride (or NULL): when
 * set, the object carries that UID instead of the synthesized palm-<uid>@cyd --
 * used to preserve a foreign object's own UID on push. Returns byte length or -1. */
static int emit_object(int kind,const uint8_t*data,int len,uint32_t uid,char*out,int cap,
                       const char*uidOverride){
    int n;
    if(kind==KIND_CAL){
        Appt a; if(ApptUnpack(data,len,&a)) return -1;
        char v[4096]; ical_emit(v,sizeof v,&a,uid);
        char vt[1200]; int vl=(a.hasTime)?ical_vtimezone(vt,sizeof vt):0; if(vl<0)vl=0; if(!vl)vt[0]=0;
        n=snprintf(out,cap,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//CYD-Palm-Bridge//EN\r\n%s%sEND:VCALENDAR\r\n",vt,v);
    } else if(kind==KIND_TODO){
        Todo t; if(ToDoUnpack(data,len,&t)) return -1;
        char v[2048]; vtodo_emit(v,sizeof v,&t,uid);
        n=snprintf(out,cap,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//CYD-Palm-Bridge//EN\r\n%sEND:VCALENDAR\r\n",v);
    } else {
        Addr a; if(AddrUnpack(data,len,&a)) return -1;
        n=vcard_emit(out,cap,&a,uid);
    }
    if(n<0) return -1;
    if(uidOverride && uidOverride[0]) setUidLine(out,cap,uidOverride);
    return (int)strlen(out);
}
/* server object bytes -> packed Palm record (inverse of emit_object). len or -1. */
static int parse_object(int kind,const char*obj,uint8_t*out,int cap){
    if(kind==KIND_CAL){ Appt a; if(ical_parse(obj,&a)) return -1; return ApptPack(out,cap,&a); }
    if(kind==KIND_TODO){ Todo t; if(vtodo_parse(obj,&t)) return -1; return ToDoPack(out,cap,&t); }
    Addr a; if(vcard_parse(obj,&a)) return -1; return AddrPack(out,cap,&a);
}

static int pushRec(const PdbRec*r,int i,void*ctx){
    (void)i; PushCtx*p=ctx;
    int bl=emit_object(p->kind,r->data,r->len,r->uniqueID,g_body,BODY_CAP,NULL);
    if(bl<0) return 0;
    FILE*f=fopen(BODY_TMP,"wb"); fwrite(g_body,1,strlen(g_body),f); fclose(f);
    char name[64]; snprintf(name,sizeof name,"%u.%s",(unsigned)r->uniqueID,kindExt(p->kind));
    char etag[160]=""; int st=0;
    put_guard(p->d,p->coll,name,kindCType(p->kind),BODY_TMP,NULL,etag,sizeof etag,&st);
    fprintf(p->map,"%d\t%u\t%s\t%s\n",p->kind,(unsigned)r->uniqueID,name,etag);
    p->n++;
    return 0;
}

int sync_push(const DavCtx*d,const char*pdbpath,const char*coll,int kind){
    if(!scratch_alloc()) return -1;
    FILE*map=fopen(STATE_DIR "/sync_map.tsv","a");
    PushCtx p={ .d=d,.coll=coll,.kind=kind,.map=map,.n=0 };
    pdb_read(pdbpath,pushRec,&p);
    if(map) fclose(map);
    return p.n;
}

typedef struct { char names[NAMEL_MAX][256]; int n; } NameList;
static void collectName(const char*name,const char*etag,void*ctx){
    (void)etag; NameList*l=ctx; if(l->n<NAMEL_MAX) snprintf(l->names[l->n++],256,"%s",name);
}

int sync_pull(const DavCtx*d,const char*coll,const char*outpdb,int kind){
    if(!scratch_alloc()) return -1;
    NameList *nl = calloc(1,sizeof *nl);
    uint8_t *arena = malloc(ARENA_CAP);
    PdbRec *recs = calloc(MAXR,sizeof *recs);
    if(!nl || !arena || !recs){ free(nl); free(arena); free(recs); fprintf(stderr,"sync_pull: out of memory\n"); return -1; }
    dav_list(d,coll,collectName,nl);
    int nrec=0, used=0;
    const char*ext=kindExt(kind);
    for(int i=0;i<nl->n;i++){
        const char*nm=nl->names[i]; const char*dot=strrchr(nm,'.'); if(!dot) continue;
        if(strcmp(dot+1,ext)) continue;                 /* keep only matching kind */
        char obj[16384]; if(dav_get(d,coll,nm,obj,sizeof obj)<=0) continue;
        uint32_t uid=(uint32_t)strtoul(nm,NULL,10);
        uint8_t*dst=arena+used; int l=parse_object(kind,obj,dst,PALM_REC_MAX);
        if(l<=0) continue;
        recs[nrec]=(PdbRec){ .attr=0,.uniqueID=uid,.data=dst,.len=l }; used+=l; nrec++;
        if(nrec>=MAXR||used+PALM_REC_MAX>ARENA_CAP) break;
    }
    for(int i=1;i<nrec;i++){ PdbRec k=recs[i]; int j=i-1;
        while(j>=0 && recs[j].uniqueID>k.uniqueID){ recs[j+1]=recs[j]; j--; } recs[j+1]=k; }
    kindWrite(kind,outpdb,NULL,0,recs,nrec);
    free(nl); free(arena); free(recs);
    return nrec;
}

/* ==================== incremental conflict-aware sync =================== */

/* optional progress hook (see sync.h). done increments per reconciled record. */
static SyncProgressFn g_progFn; static void *g_progCtx;
static int g_progTotal, g_progDone;
void sync_set_progress(SyncProgressFn fn,void*ctx){ g_progFn=fn; g_progCtx=ctx; }
static void progReset(int total){ g_progTotal=total; g_progDone=0;
    if(g_progFn) g_progFn(0,total,g_progCtx); }
static void progTick(void){ g_progDone++;
    if(g_progFn) g_progFn(g_progDone,g_progTotal,g_progCtx); }

static uint64_t fnv1a(const char*s){
    uint64_t h=1469598103934665603ULL;
    for(;*s;s++){ h^=(uint8_t)*s; h*=1099511628211ULL; }
    return h;
}
static uint32_t nameToUid(const char*name){
    char*end; unsigned long v=strtoul(name,&end,10);
    if(end!=name && (*end=='.'||*end==0)) return (uint32_t)v;
    return (uint32_t)(fnv1a(name) & 0xFFFFFF);   /* stable id for foreign hrefs */
}
/* reconciliation identity: a 64-bit hash of an object's iCal/vCard UID. This
 * (not the href name) is what matches a local record to its server object, so a
 * relocated href or a lost map row doesn't split one record into two. */
static uint64_t uidHash(const char*u){ return fnv1a(u); }
/* the UID a never-synced local record will carry: palm-<uid>@cyd (what the
 * emitters synthesize), so its identity is stable from the very first push. */
static uint64_t synthHash(uint32_t uid){
    char b[32]; snprintf(b,sizeof b,"palm-%u@cyd",(unsigned)uid); return uidHash(b);
}

/* ---- streaming reconciliation (no fixed per-record RAM cap) ----------------
 * The old engine held loc[MAXR]/map[MAXR]/srv[MAXR] resident WHILE making DAV
 * calls, so the reconcile working set had to coexist with the ~35 KB mbedTLS
 * handshake in the fragmented no-PSRAM heap -- that coexistence is what capped a
 * collection at 24 records. This engine instead materializes three index files
 * on disk, each keyed by the object's UID hash, sorts them (with no handshake
 * live, so O(N) sorting can use the full free block), then MERGE-JOINS them:
 * during every DAV op only the current UID's row is resident, so peak RAM during
 * TLS is O(1) in the record count. See sync_one below.
 *
 * `davreq` (dav_esp.c) does init->perform->cleanup per call, so mbedTLS is only
 * live *inside* a DAV call; the sorts/joins between calls are handshake-free. */
typedef struct {                       /* local record index row (bytes read lazily) */
    uint32_t uid; uint8_t attr; int pdbIdx; int len; uint64_t hash;
} Loc;

typedef struct {
    int kind;
    char pdbpath[256];                 /* source PDB for lazy local record reads */
    char token[1408];                  /* RFC 6578 sync-token from last run   */
    char newToken[1408];               /* token to persist after this run     */
} S;

/* read one local record's bytes on demand (returns len, or -1). */
static int locBytes(const S*s,const Loc*L,uint8_t*buf,int cap){
    return pdb_read_one(s->pdbpath,L->pdbIdx,buf,cap,NULL,NULL);
}

/* fetch buffer for one server object. iCloud contacts can embed a base64 PHOTO
 * that pushes a vCard well past 16 KB; too small a buffer truncates the object
 * (no END:VCARD) and it fails to parse. Generous on host; the no-PSRAM device
 * keeps it small (an over-limit object is skipped with a warning). One shared
 * static (used by resolveServer + keepFromServer, never concurrently) keeps BSS
 * flat. */
#ifndef OBJ_FETCH_CAP
#ifdef ESP_PLATFORM
#define OBJ_FETCH_CAP (8*1024)
#else
#define OBJ_FETCH_CAP (256*1024)
#endif
#endif
static char *g_objbuf;    /* server-object fetch buffer (OBJ_FETCH_CAP) */
/* The per-run reconcile state. It lives in the scratch pool rather than being
 * calloc'd per entry so that (a) it is taken while the heap is still whole and
 * (b) the second and later collections of a run reuse it instead of hunting for
 * a fresh 3 KB block in a heap the first collection just carved up. The two
 * entry points never nest (sync_categorized drives sync_one directly), so one
 * instance is enough. */
static S *g_state;

/* ---- the budget (contract in sync.h) --------------------------------------
 * SYNC_WORKING_SET is every byte scratch_alloc will ask the heap for, named as
 * one number so it can be compared against a ceiling instead of discovered one
 * failed malloc at a time. */
#define SYNC_WORKING_SET ((size_t)OBJ_FETCH_CAP + (size_t)BODY_CAP + \
                          (size_t)PALM_REC_MAX  + sizeof(S))
static size_t s_budget;          /* 0 = unlimited (host build, gates) */
void   sync_set_budget(size_t bytes){ s_budget = bytes; }
static int  s_too_big;
static long s_too_big_bytes;
/* Ceiling on a single in-RAM sort. 0 = whatever the allocator will give. Set it
 * to refuse a collection deliberately rather than discovering the limit by
 * failing a malloc, and to let the gates exercise the refusal without having to
 * stage a real out-of-memory. */
static long s_max_sort;
void   sync_set_max_sort(long bytes){ s_max_sort = bytes; }

size_t sync_working_set(void){ return SYNC_WORKING_SET; }
long   sync_too_big_bytes(void){ return s_too_big_bytes; }
/* ---- pull-only (contract in sync.h) ---------------------------------------
 * While the memory rework is in flight the engine must not be able to write to
 * the account. A bug in a half-converted reconcile path would corrupt real data
 * on the server, and unlike a local PDB that is not something a reflash undoes.
 * Writes are refused at the points that reach the network, and the existing
 * "push failed -- kept local, will retry" handling takes it from there, so no
 * new untested branch is introduced on the path that matters. */
static SyncCheckFn s_check;
void sync_set_check(SyncCheckFn fn){ s_check = fn; }
#define CHK(w) do{ if(s_check) s_check(w); }while(0)
static void chk_n(const char *tag,int n){
    if(!s_check) return;
    char b[40]; snprintf(b,sizeof b,"%s#%d",tag,n); s_check(b);
}

static int s_pull_only;
void sync_set_pull_only(int on){ s_pull_only = on; }

static SyncHoldFn s_hold;
static void      *s_hold_ctx;
void sync_set_hold(SyncHoldFn fn, void *ctx){ s_hold = fn; s_hold_ctx = ctx; }
int  sync_pull_only(void){ return s_pull_only; }

static int put_guard(const DavCtx*d,const char*coll,const char*name,const char*ctype,
                     const char*bodyfile,const char*ifmatch,
                     char*etag,int etagcap,int*status){
    if(s_pull_only){
        if(etag && etagcap) etag[0]=0;
        if(status) *status=0;              /* reads as a failed push: keep local */
        return 0;
    }
    return dav_put(d,coll,name,ctype,bodyfile,ifmatch,etag,etagcap,status);
}
static int del_guard(const DavCtx*d,const char*coll,const char*name,const char*ifmatch){
    if(s_pull_only) return 0;              /* server keeps the record; next run re-pulls it */
    return dav_delete(d,coll,name,ifmatch);
}

static int over_budget(void){
    if(!s_budget || SYNC_WORKING_SET <= s_budget) return 0;
    fprintf(stderr,"[sync] REFUSED before any network I/O: working set %u B "
                   "exceeds the %u B budget\n",
            (unsigned)SYNC_WORKING_SET, (unsigned)s_budget);
    return 1;
}

/* Acquire/release the sync scratch. scratch_alloc() runs at each public sync
 * entry; it is idempotent, so a nested entry point (sync_categorized ->
 * sync_collection) shares one allocation. sync_free_scratch() hands the ~20 KB
 * back -- the device calls it after a HotSync so interactive mode gets the RAM;
 * the host CLI/tests may skip it (the process exits). Returns 1 ok, 0 on OOM. */
static int scratch_alloc(void){
    /* LARGEST FIRST, and the order is load-bearing. Measured on device with a
     * live TLS session the heap had ~25.5 KB free but only 17408 of it in one
     * run, so the two 8 KB blocks are the only ones with nowhere else to go:
     * they must be placed while the big run is still whole, and the 4 KB and
     * 3 KB blocks can then take the remnants. Interleaved sizes (the original
     * order) and smallest-first were both measured on device and both stranded
     * a block -- smallest-first merely moved the failure from the 3 KB state to
     * the last 8 KB buffer. */
    if(!g_objbuf) g_objbuf = malloc(OBJ_FETCH_CAP);
    if(!g_body)   g_body   = malloc(BODY_CAP);
    if(!g_lrec)   g_lrec   = malloc(PALM_REC_MAX);
    if(!g_state)  g_state  = malloc(sizeof(S));
    if(g_state && g_body && g_lrec && g_objbuf) return 1;
    /* WHICH block could not be found matters more than the total. These are three
     * separate contiguous requests, the two 8 KB ones being the hard asks; on a
     * fragmented heap the total free can look healthy while no 8 KB run exists.
     * Naming the failed block turns "out of memory" into an actionable number. */
    fprintf(stderr,"sync: scratch_alloc FAILED -- objbuf(%d)=%s body(%d)=%s lrec(%d)=%s state(%d)=%s\n",
            (int)OBJ_FETCH_CAP, g_objbuf ? "ok" : "FAILED",
            (int)BODY_CAP,      g_body   ? "ok" : "FAILED",
            (int)PALM_REC_MAX,  g_lrec   ? "ok" : "FAILED",
            (int)sizeof(S),     g_state  ? "ok" : "FAILED");
    return 0;
}
/* ---- the bulk half of the scratch ------------------------------------------
 * g_body (emit) and g_objbuf (object fetch) are 8 KB each, and NEITHER is in
 * use during the server enumeration: that phase streams the reply to SD and
 * parses it in its own window. Holding them across it is nevertheless what made
 * the enumeration fail on the largest collection -- mbedTLS wants a receive
 * buffer of SSL_IN_CONTENT_LEN (measured on device: "alloc(16749 bytes)
 * failed", REPORT truncated at 15631 of 41496 bytes), and these two are exactly
 * that much heap lying idle at that moment.
 *
 * So they are handed back for the enumeration and taken again at the first
 * phase that actually needs each one: g_objbuf for resolveServer's GETs,
 * g_body for buildLcRaw's hashing. Same trick the news fetch uses for its file
 * handles, and the same reason: the memory is wanted by the handshake. */
static void bulk_free(void){
    free(g_body);   g_body=NULL;
    free(g_objbuf); g_objbuf=NULL;
}
static int need_objbuf(void){ if(!g_objbuf) g_objbuf = malloc(OBJ_FETCH_CAP); return g_objbuf!=NULL; }
static int need_body(void){   if(!g_body)   g_body   = malloc(BODY_CAP);      return g_body!=NULL; }

void sync_free_scratch(void){
    free(g_body);   g_body=NULL;
    free(g_lrec);   g_lrec=NULL;
    free(g_objbuf); g_objbuf=NULL;
    free(g_state);  g_state=NULL;
}

/* per-collection index temp files (STATE_DIR). Each is rebuilt per collection;
 * sync is single-threaded so sharing the names across collections is fine. */
#define MP_IDX  STATE_DIR "/.mp.idx"   /* map rows,     key=objhash */
#define MP_HREF STATE_DIR "/.mp.href"  /* href -> objhash,objuid,etag (key=href)     */
#define MP_PALM STATE_DIR "/.mp.palm"  /* palmuid -> objhash        (key=palmuid)    */
#define LC_RAW  STATE_DIR "/.lc.raw"   /* local recs,   key=palmuid */
#define LC_IDX  STATE_DIR "/.lc.idx"   /* local recs,   key=objhash */
#define SV_RAW  STATE_DIR "/.sv.raw"   /* server enum,  key=href     */
#define SV_IDX  STATE_DIR "/.sv.idx"   /* server objs,  key=objhash  */
#define SV_MO   STATE_DIR "/.sv.mo"    /* map-only rows, flushed once trust is known */
#define SV_PEND STATE_DIR "/.sv.pend"  /* server-only objects: name etag (key=href)  */
#define NEED    STATE_DIR "/.need"     /* names whose bodies the merge will want     */
#define BC_DAT  STATE_DIR "/.bc.dat"   /* fetched object bodies, back to back        */
#define BC_IDX  STATE_DIR "/.bc.idx"   /* name -> offset,length (fixed width, key=name) */
#define MG_SPOOL STATE_DIR "/.mget"    /* one multiget reply                         */

/* ---- the time window (contract in sync.h) ---- */
static long long s_win_start, s_win_end;
void sync_set_window(long long start, long long end){ s_win_start = start; s_win_end = end; }

/* compare two lines by their first field (up to the first TAB). Every index
 * file leads with its sort key zero-padded (objhash %016llx / palmuid %010u) or
 * an href string, so a lexical first-field sort is the intended order. */
static int cmpLine(const void*a,const void*b){
    const char*x=*(const char*const*)a,*y=*(const char*const*)b;
    for(;;){
        char cx=*x,cy=*y;
        int ex=(cx=='\t'||cx=='\n'||cx==0), ey=(cy=='\t'||cy=='\n'||cy==0);
        if(ex||ey) return ex==ey?0:(ex?-1:1);
        if(cx!=cy) return (int)(unsigned char)cx-(int)(unsigned char)cy;
        x++;y++;
    }
}
/* ---- "this collection is bigger than this device" ---------------------------
 * Set when a step fails for SIZE rather than for a transient shortage: a line
 * too long for the sort's buffer, or an output index that will not grow. It is
 * kept apart from an ordinary OOM because the user needs different advice --
 * retrying will not help, and the honest answer is that this collection cannot
 * be synced here.
 *
 * Both failures must be loud, because silent they are WORSE THAN A CRASH. A
 * file left unsorted and walked by the merge-join as if sorted mis-pairs
 * records into spurious deletes and duplicates; a record dropped from the
 * merged PDB is read by the NEXT sync as locally deleted, and that deletion is
 * pushed to the server. */

/* ---- the sort: in RAM when it fits, in runs on the card when it doesn't ----
 * Every index file is sorted by its first field. A file that fits the sort's
 * buffer is sorted there in one go. A larger one -- a real account's calendar
 * is easily tens of kilobytes of index -- is cut into sorted RUNS of one buffer
 * each, written to the card, and then merged SORT_MERGE_K runs at a time, in as
 * many passes as it takes, back into the file. RAM is the buffer plus K line
 * buffers whatever the file's size; the card pays instead.
 *
 * The buffer is s_max_sort bytes when that is set (the gates set it small to
 * force the run path), else SORT_RUN_MAX. If even a smaller buffer can't be
 * had, the sort halves its request down to SORT_RUN_MIN before giving up. */
#ifdef ESP_PLATFORM
#define SORT_RUN_MAX  4096
#else
#define SORT_RUN_MAX  (1L<<20)
#endif
#define SORT_RUN_MIN  1024
#define SORT_LINE_MAX 512                  /* longest line any index file holds */
#define SORT_MERGE_K  4                    /* runs merged at once (open files)  */
#define SORT_RUN_FMT  STATE_DIR "/.srt%d.%d"

/* Fill `buf` with whole lines from `f` (up to cap bytes, at most maxl lines),
 * pointers in `lines`. Returns the number of lines; *eof when the file ended.
 * A line that doesn't fit is left for the next run; -1 if a single line is
 * longer than the whole buffer (or than SORT_LINE_MAX). */
static int sortReadRun(FILE*f,char*buf,long cap,char**lines,int maxl,int*eof){
    long used=0; int n=0; *eof=0;
    while(n<maxl){
        long at=ftell(f);
        long room=cap-used;
        if(room<2) break;
        if(!fgets(buf+used,(int)room,f)){ *eof=1; break; }
        long l=(long)strlen(buf+used);
        int whole = l>0 && buf[used+l-1]=='\n';
        if(!whole){
            int c=fgetc(f);
            if(c==EOF){                    /* the last line, with no newline */
                if(used+l+2>cap){ if(!n) return -1; fseek(f,at,SEEK_SET); break; }
                buf[used+l]='\n'; buf[used+l+1]=0; l++; *eof=1;
            } else {                       /* didn't fit: carry it to the next run */
                if(!n) return -1;
                fseek(f,at,SEEK_SET); break;
            }
        }
        if(l>SORT_LINE_MAX-1) return -1;     /* the merge reads lines into SORT_LINE_MAX */
        lines[n++]=buf+used; used+=l+1;   /* keep the NUL as the separator */
        if(*eof) break;
    }
    if(n==maxl){ int c=fgetc(f); if(c==EOF) *eof=1; else ungetc(c,f); }
    return n;
}

/* Merge the sorted line files in[0..k-1] into out. 0 ok, -1 on a write error. */
static int sortMerge(FILE**in,int k,FILE*out,char*lb){
    int have[SORT_MERGE_K];
    for(int i=0;i<k;i++) have[i] = fgets(lb+i*SORT_LINE_MAX,SORT_LINE_MAX,in[i])!=NULL;
    for(;;){
        int best=-1;
        for(int i=0;i<k;i++){
            if(!have[i]) continue;
            const char*a=lb+i*SORT_LINE_MAX;
            if(best<0){ best=i; continue; }
            const char*b=lb+best*SORT_LINE_MAX;
            if(cmpLine(&a,&b)<0) best=i;
        }
        if(best<0) return 0;
        if(fputs(lb+best*SORT_LINE_MAX,out)<0) return -1;
        have[best] = fgets(lb+best*SORT_LINE_MAX,SORT_LINE_MAX,in[best])!=NULL;
    }
}

static void sortFail(const char*path,const char*why,long bytes){
    fprintf(stderr,"[sync] SORT FAILED for %s: %s.\n"
                   "       Refusing -- merging against UNSORTED input mis-pairs records.\n",path,why);
    s_too_big = 1; if(bytes > s_too_big_bytes) s_too_big_bytes = bytes;
}

int sync_sort_file(const char*path){
    /* The connection stays up across a sort. Disconnecting first, so the sort
     * needn't fight the ~40 KB TLS working set, is the wrong trade (measured
     * on device): the sort needs a few KB, while the reconnect it forces
     * needs a ~30 KB handshake peak, which can't be mounted with the sync
     * scratch held ("alloc(4770 bytes) failed"). If the server drops it anyway the
     * reconnect is best-effort, and the circuit breaker (dav.h) ends the
     * collection in seconds with local data untouched. */
    FILE*f=fopen(path,"rb"); if(!f) return 1;          /* absent == nothing to sort */
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0){ fclose(f); return 1; }
    long cap = s_max_sort > 0 ? s_max_sort : SORT_RUN_MAX;
    if(cap > sz+2) cap = sz+2;                          /* never more than the file needs */
    char*buf=NULL; char**lines=NULL; int maxl=0;
    for(;;){
        maxl = (int)(cap/16) + 4;                       /* the shortest line is ~28 bytes */
        buf = malloc((size_t)cap);
        lines = buf ? malloc((size_t)maxl*sizeof*lines) : NULL;
        if(buf && lines) break;
        free(buf); free(lines); buf=NULL; lines=NULL;
        if(s_max_sort > 0 || cap/2 < SORT_RUN_MIN){
            fclose(f); sortFail(path,"no RAM for the sort buffer",cap); return 0; }
        cap/=2;
    }
    int eof=0, nruns=0, ok=1;
    for(;;){
        int n=sortReadRun(f,buf,cap,lines,maxl,&eof);
        if(n<0){ ok=0; sortFail(path,"a line longer than the sort buffer",cap); break; }
        qsort(lines,n,sizeof*lines,cmpLine);
        if(eof && nruns==0){                           /* it all fit: write it back */
            fclose(f); f=NULL;
            FILE*o=fopen(path,"wb");
            if(!o){ ok=0; break; }
            for(int i=0;i<n;i++) if(fputs(lines[i],o)<0){ ok=0; break; }
            if(fclose(o)!=0) ok=0;
            if(!ok) sortFail(path,"could not rewrite it",0);
            free(lines); free(buf);
            return ok;
        }
        char rp[96]; snprintf(rp,sizeof rp,SORT_RUN_FMT,0,nruns);
        FILE*o=fopen(rp,"wb");
        if(!o){ ok=0; sortFail(path,"could not write a run to the card",0); break; }
        for(int i=0;i<n;i++) if(fputs(lines[i],o)<0){ ok=0; break; }
        if(fclose(o)!=0) ok=0;
        if(!ok){ sortFail(path,"could not write a run to the card",0); break; }
        nruns++;
        if(eof) break;
    }
    if(f) fclose(f);
    free(lines); free(buf);
    /* merge: pass p reads runs (p, 0..n-1) and writes runs (p+1, ...), K at a
     * time, until one pass writes the file itself */
    char*lb = ok ? malloc((size_t)SORT_MERGE_K*SORT_LINE_MAX) : NULL;
    if(ok && !lb){ ok=0; sortFail(path,"no RAM for the merge",(long)SORT_MERGE_K*SORT_LINE_MAX); }
    int pass=0;
    while(ok){
        int outs=(nruns+SORT_MERGE_K-1)/SORT_MERGE_K, last=(outs==1);
        for(int g=0; g<outs && ok; g++){
            FILE*in[SORT_MERGE_K]; int k=0;
            for(int r=g*SORT_MERGE_K; r<nruns && k<SORT_MERGE_K; r++){
                char rp[96]; snprintf(rp,sizeof rp,SORT_RUN_FMT,pass,r);
                in[k]=fopen(rp,"rb"); if(!in[k]){ ok=0; break; } k++;
            }
            char op[96]; if(last) snprintf(op,sizeof op,"%s",path); else snprintf(op,sizeof op,SORT_RUN_FMT,pass+1,g);
            FILE*o = ok ? fopen(op,"wb") : NULL;
            if(!o) ok=0;
            if(ok && sortMerge(in,k,o,lb)!=0) ok=0;
            if(o && fclose(o)!=0) ok=0;
            for(int i=0;i<k;i++) fclose(in[i]);
            for(int r=g*SORT_MERGE_K; r<nruns && r<(g+1)*SORT_MERGE_K; r++){
                char rp[96]; snprintf(rp,sizeof rp,SORT_RUN_FMT,pass,r); remove(rp); }
            if(!ok) sortFail(path,"a merge pass failed on the card",0);
        }
        if(last) break;
        nruns=outs; pass++;
    }
    free(lb);
    /* on failure, sweep any runs left behind */
    if(!ok) for(int p2=0;p2<=pass+1;p2++) for(int r=0;;r++){
        char rp[96]; snprintf(rp,sizeof rp,SORT_RUN_FMT,p2,r); if(remove(rp)!=0) break; }
    return ok;
}
static int sortFile(const char*path){ return sync_sort_file(path); }

/* Stream the on-disk map into three sorted views: by objhash (MP_IDX, a reconcile
 * source), by href (MP_HREF, to resolve server objects without a GET), and by
 * palmuid (MP_PALM, to give local records their objhash). Reads the persisted
 * sync-token into *token; tracks the max palmuid for POL_BOTH fork ids. */
static void buildMapIdx(const char*mapfile,char*token,int tokcap,uint32_t*maxuid){
    token[0]=0;
    FILE*f=fopen(mapfile,"r");
    FILE*a=fopen(MP_IDX,"w"),*b=fopen(MP_HREF,"w"),*c=fopen(MP_PALM,"w");
    if(f){
        char line[1400];
        while(fgets(line,sizeof line,f)){
            if(!strncmp(line,"#synctoken\t",11)){
                char*t=line+11; size_t l=strlen(t); while(l&&(t[l-1]=='\n'||t[l-1]=='\r'))t[--l]=0;
                snprintf(token,tokcap,"%s",t); continue;
            }
            unsigned mu=0; char href[64]="",etag[160]="",objuid[48]=""; unsigned long long h=0;
            int nf=sscanf(line,"%u\t%63[^\t]\t%159[^\t]\t%llu\t%47[^\t\r\n]",&mu,href,etag,&h,objuid);
            if(nf<3) continue;
            if(nf<5) objuid[0]=0;
            uint64_t oh = objuid[0]?uidHash(objuid):synthHash(mu);
            if(a) fprintf(a,"%016llx\t%u\t%s\t%s\t%llu\t%s\n",(unsigned long long)oh,mu,href,etag,h,objuid);
            if(b) fprintf(b,"%s\t%016llx\t%s\t%s\n",href,(unsigned long long)oh,objuid,etag);
            if(c) fprintf(c,"%010u\t%016llx\n",mu,(unsigned long long)oh);
            if(mu>*maxuid)*maxuid=mu;
        }
        fclose(f);
    }
    if(a){fclose(a);} if(b){fclose(b);} if(c){fclose(c);}
    CHK("map-built"); sortFile(MP_IDX); sortFile(MP_HREF); sortFile(MP_PALM); CHK("map-sorted");
}

/* Trip the mass-delete guard only once a collection is big enough for "most of
 * it vanished" to mean anything. Below this, a small collection legitimately
 * emptying is ordinary. */
#define MASSDEL_MIN 8
static int countLines(const char*path){
    FILE*f=fopen(path,"r"); if(!f) return 0;
    int n=0, c, last='\n';
    while((c=fgetc(f))!=EOF){ if(c=='\n') n++; last=c; }
    if(last!='\n') n++;               /* a final line without a newline still counts */
    fclose(f);
    return n;
}

/* build LC_RAW (key=palmuid) from the source PDB, optionally filtered to records
 * that route to collection C (category sync). The hash is over the synth-UID
 * body (UID-independent) so it flags only real local content changes. */
typedef struct { FILE*f; int kind; const CatRoute*rt; const char*C; uint32_t maxuid; } LcBuild;
static int lcBuildCb(const PdbRec*r,int i,void*ctx){
    LcBuild*b=ctx;
    if(b->rt){
        int cat=r->attr&REC_ATTR_CAT;
        const char*dest=b->rt->coll[cat]?b->rt->coll[cat]:b->rt->def;
        if(!dest||strcmp(dest,b->C)) return 0;      /* not this collection */
    }
    uint64_t hash=0;
    if(emit_object(b->kind,r->data,r->len,r->uniqueID,g_body,BODY_CAP,NULL)>=0) hash=fnv1a(g_body);
    if(b->f) fprintf(b->f,"%010u\t%d\t%d\t%d\t%llu\n",
                     (unsigned)r->uniqueID,i,(int)r->attr,r->len,(unsigned long long)hash);
    if(r->uniqueID>b->maxuid) b->maxuid=r->uniqueID;
    return 0;
}
static void buildLcRaw(const char*pdbpath,int kind,const CatRoute*rt,const char*C,uint32_t*maxuid){
    LcBuild b={ .kind=kind,.rt=rt,.C=C,.maxuid=*maxuid };
    b.f=fopen(LC_RAW,"w");
    pdb_read(pdbpath,lcBuildCb,&b);
    if(b.f)fclose(b.f);
    *maxuid=b.maxuid;
    CHK("lc-raw-built"); sortFile(LC_RAW); CHK("lc-raw-sorted");
}
/* join LC_RAW (key palmuid) with MP_PALM (key palmuid) -> LC_IDX (key objhash).
 * A local record with no map row (never synced) gets synthHash(palmuid). */
static void joinLcIdx(void){
    FILE*l=fopen(LC_RAW,"r"),*m=fopen(MP_PALM,"r"),*o=fopen(LC_IDX,"w");
    char ll[512],ml[64]; int haveL=l&&fgets(ll,sizeof ll,l), haveM=m&&fgets(ml,sizeof ml,m);
    unsigned mu=0; unsigned long long moh=0; int mok=haveM&&sscanf(ml,"%u\t%llx",&mu,&moh)>=2;
    while(haveL){
        unsigned lu=0; int idx=0,attr=0,len=0; unsigned long long h=0;
        if(sscanf(ll,"%u\t%d\t%d\t%d\t%llu",&lu,&idx,&attr,&len,&h)<5){ haveL=l&&fgets(ll,sizeof ll,l)!=NULL; continue; }
        while(haveM && mok && mu<lu){ haveM=fgets(ml,sizeof ml,m)!=NULL; mok=haveM&&sscanf(ml,"%u\t%llx",&mu,&moh)>=2; }
        uint64_t oh = (haveM && mok && mu==lu) ? (uint64_t)moh : synthHash(lu);
        if(o) fprintf(o,"%016llx\t%u\t%d\t%d\t%d\t%llu\n",(unsigned long long)oh,lu,idx,attr,len,h);
        haveL=l&&fgets(ll,sizeof ll,l)!=NULL;
    }
    if(l){fclose(l);} if(m){fclose(m);} if(o){fclose(o);}
    CHK("lc-idx-built"); sortFile(LC_IDX); CHK("lc-idx-sorted");
}

/* enumerate the server into SV_RAW (key=href): "href etag present". Prefers the
 * RFC 6578 delta (only changed/deleted; the unchanged baseline comes from the
 * map) and falls back to a full REPORT/PROPFIND. *incremental tells resolveServer
 * how to read a map row that has no server row (unchanged vs. deleted). */
typedef struct { FILE*f; } RawEnum;
static void rawReportCb(const char*name,const char*etag,int deleted,void*ctx){
    FILE*f=((RawEnum*)ctx)->f; if(f) fprintf(f,"%s\t%s\t%d\n",name,deleted?"":etag,deleted?0:1);
}
static void rawListCb(const char*name,const char*etag,void*ctx){
    FILE*f=((RawEnum*)ctx)->f; if(f) fprintf(f,"%s\t%s\t1\n",name,etag);
}
/* truncate-reopen a stream (fclose+fopen, not freopen -- FATFS/VFS on the device
 * doesn't reliably support freopen). Used to discard a partial enumeration. */
static FILE* reopenTrunc(FILE*f,const char*path){
    if(f) fclose(f);
    return fopen(path,"w");
}
/* Returns 0 if SV_RAW is an AUTHORITATIVE server view (a delta, a full report, or
 * a PROPFIND all succeeded), or -1 if every attempt failed/was truncated. On -1
 * the caller must NOT treat the (empty/partial) SV_RAW as "the server deleted
 * everything" -- see sync_one. */
static int enumServer(const DavCtx*d,const char*coll,int kind,const char*token,
                      char*newtok,int tokcap,int*incremental,int*windowed){
    *incremental=0; *windowed=0; newtok[0]=0;
    RawEnum re; re.f=fopen(SV_RAW,"w");
    int done=0, ok=0;
    /* A calendar with a window: list only what's in it. The listing is the
     * whole truth about the window, so it is never a delta; an object missing
     * from it is "not listed", which the merge treats as outside the window,
     * not as deleted (see sync.h). A server that doesn't answer the query is
     * enumerated whole, as before. */
    if(kind==KIND_CAL && s_win_end > s_win_start){
        if(dav_query_window(d,coll,s_win_start,s_win_end,rawListCb,&re)>=0){
            if(re.f) fclose(re.f);
            *windowed=1;
            return 0;
        }
        fprintf(stderr,"[sync] %s: the window query wasn't answered -- syncing the whole collection\n",coll);
        re.f=reopenTrunc(re.f,SV_RAW);
    }
    if(token[0]){
        int rc=dav_sync_report(d,coll,token,rawReportCb,&re,newtok,tokcap);
        if(rc==0){ done=1; ok=1; *incremental=1; }
        else { newtok[0]=0; re.f=reopenTrunc(re.f,SV_RAW); }   /* discard partial delta */
    }
    if(!done){
        int rc=dav_sync_report(d,coll,"",rawReportCb,&re,newtok,tokcap);
        if(rc==0) ok=1;                                        /* full report authoritative */
        else {                                                 /* fall back to PROPFIND (lighter: etags, no bodies) */
            newtok[0]=0; re.f=reopenTrunc(re.f,SV_RAW);
            if(dav_list(d,coll,rawListCb,&re)>=0) ok=1;
        }
    }
    if(re.f) fclose(re.f);
    return ok ? 0 : -1;
}
/* ---- the body cache: objects fetched in batches -----------------------------
 * The merge wants the body of every object that is new or changed on the
 * server. Fetching them one GET at a time is a round trip each (and a new
 * object used to take two: one for its UID, one for its body). prefetch()
 * asks for them in batches with a multiget, streams each reply onto the card,
 * and indexes the bodies by name in fixed-width rows, so a lookup is a binary
 * search with no RAM to speak of. Anything not in the cache (a server that
 * doesn't do multiget, a name too long to index) falls back to a GET. */
#define BC_KEY   127                        /* names longer than this aren't cached */
#define BC_W     (BC_KEY+1+10+1+10+1)       /* one index row, newline included      */
#define MG_BATCH 16                         /* objects per multiget                 */

typedef struct { FILE*idx; } BcSink;
static int s_fetch_batched, s_fetch_single;
void sync_fetch_counts(int *batched, int *single){
    if(batched) *batched = s_fetch_batched;
    if(single)  *single  = s_fetch_single;
}
static void bcCb(const char*name,const char*etag,long off,long len,void*ctx){
    (void)etag; BcSink*b=ctx;
    if(b->idx && strlen(name)<=BC_KEY) fprintf(b->idx,"%-127s\t%010ld\t%010ld\n",name,off,len);
}
static void bcReset(void){ remove(BC_DAT); remove(BC_IDX); remove(NEED); remove(SV_PEND); remove(MG_SPOOL); }

/* the cached body of `name` into out (NUL-terminated); its length (capped at
 * cap-1, which callers read as "too big"), or -1 if it isn't cached */
static int cacheGet(const char*name,char*out,int cap){
    if(strlen(name)>BC_KEY) return -1;
    FILE*f=fopen(BC_IDX,"rb"); if(!f) return -1;
    fseek(f,0,SEEK_END); long n=ftell(f)/BC_W;
    char key[BC_KEY+1]; snprintf(key,sizeof key,"%-127s",name);
    long lo=0, hi=n-1, off=-1, len=0;
    char row[BC_W+1];
    while(lo<=hi){
        long mid=(lo+hi)/2;
        if(fseek(f,mid*BC_W,SEEK_SET) || fread(row,1,BC_W,f)!=BC_W) break;
        int c=memcmp(row,key,BC_KEY);
        if(c==0){ row[BC_W]=0; off=strtol(row+BC_KEY+1,NULL,10); len=strtol(row+BC_KEY+12,NULL,10); break; }
        if(c<0) lo=mid+1; else hi=mid-1;
    }
    fclose(f);
    if(off<0) return -1;
    FILE*d=fopen(BC_DAT,"rb"); if(!d) return -1;
    long want = len < cap-1 ? len : cap-1;
    int got = (fseek(d,off,SEEK_SET)==0) ? (int)fread(out,1,(size_t)want,d) : -1;
    fclose(d);
    if(got!=want) return -1;
    out[got]=0;
    return len >= cap-1 ? cap-1 : got;
}
/* a body: from the cache, else one GET */
static int fetchBody(const DavCtx*d,const char*coll,const char*name,char*out,int cap){
    int got=cacheGet(name,out,cap);
    if(got>=0) return got;
    s_fetch_single++;
    return dav_get(d,coll,name,out,cap);
}

static void prefetch(const DavCtx*d,const char*coll,int kind){
    FILE*need=fopen(NEED,"r"); if(!need) return;
    FILE*dat=fopen(BC_DAT,"w+b"), *idx=fopen(BC_IDX,"w");
    char (*nm)[BC_KEY+1] = malloc((size_t)MG_BATCH*sizeof *nm);
    const char*np[MG_BATCH];
    if(!dat || !idx || !nm){ if(dat)fclose(dat); if(idx)fclose(idx); free(nm); fclose(need); return; }
    BcSink b={ .idx=idx };
    int batches=0, got=0, eof=0;
    while(!eof){
        int k=0; char ln[300];
        while(k<MG_BATCH){
            if(!fgets(ln,sizeof ln,need)){ eof=1; break; }
            size_t l=strlen(ln); while(l && (ln[l-1]=='\n'||ln[l-1]=='\r')) ln[--l]=0;
            if(!l || l>BC_KEY) continue;              /* too long to index: it'll be a GET */
            memcpy(nm[k],ln,l+1); np[k]=nm[k]; k++;
        }
        if(!k) break;
        int st=dav_multiget(d,coll,kind==KIND_CARD,np,k,MG_SPOOL);
        batches++;
        if(st!=207){
            /* no multiget here: everything left is fetched one at a time */
            fprintf(stderr,"[sync] %s: multiget answered %d -- fetching one by one\n",coll,st);
            break;
        }
        FILE*sp=fopen(MG_SPOOL,"rb");
        if(sp){ int n=dav_parse_multiget_stream(sp,dat,bcCb,&b); if(n>0){ got+=n; s_fetch_batched+=n; } fclose(sp); }
        remove(MG_SPOOL);
    }
    free(nm); fclose(need); fclose(dat); fclose(idx);
    SYNC_LOG("[sync] %s: prefetched %d objects in %d request(s)\n",coll,got,batches);
    CHK("bc-built"); sortFile(BC_IDX); CHK("bc-sorted");
}

/* Resolve every server object to an objhash and write SV_IDX (key=objhash):
 * "objhash href etag present". Joins SV_RAW (key href) with MP_HREF (key href):
 *   both        -> objhash from the map (no fetch); etag/present from enumeration.
 *   map only    -> unchanged (incremental) present w/ map etag; in a window's
 *                  listing "not listed" (present=2); else deleted.
 *   server only -> a new object: its body says its UID -> objhash.
 * In two passes with the batched fetch between: resolveList() needs no body and
 * writes the list of bodies the merge will want; resolvePending() reads the new
 * objects' UIDs from the fetched bodies.
 *
 * resolvePending() returns 1 if ANY server-only object could NOT be
 * UID-resolved (its fetch failed, was truncated on the small no-PSRAM fetch
 * buffer, or was unparseable), else 0. Such an object is DEFERRED, not
 * force-identified: falling back to uidHash(href) would mint a divergent
 * identity for what is really an already mapped record -- so the mapped copy
 * looked server-deleted (spurious delete) AND the object looked brand-new
 * (phantom pull). That split is the on-device duplication seen against iCloud
 * (relocated photo-vCards overflow the 8 KB buffer). Instead we skip the
 * object this round and tell the caller to SUPPRESS DELETES (so a transient
 * fetch failure can never delete the mapped local record), and it retries
 * cleanly next sync. Map-only rows are therefore staged to SV_MO and only
 * flushed as deletes once we know the enumeration was fully resolved. */
static void resolveList(void){
    CHK("sv-raw-built"); sortFile(SV_RAW); CHK("sv-raw-sorted");  /* merge-join needs it keyed (by href) */
    FILE*s=fopen(SV_RAW,"r"),*m=fopen(MP_HREF,"r"),*o=fopen(SV_IDX,"w");
    FILE*mo=fopen(SV_MO,"w");                  /* map-only rows, present decided after merge */
    FILE*pend=fopen(SV_PEND,"w"),*need=fopen(NEED,"w");
    char sl[512],ml[512];
    int haveS=s&&fgets(sl,sizeof sl,s), haveM=m&&fgets(ml,sizeof ml,m);
    while(haveS||haveM){
        char sh[128]="",se[160]=""; int sp=0;
        if(haveS) sscanf(sl,"%127[^\t]\t%159[^\t]\t%d",sh,se,&sp);
        char mh[128]=""; unsigned long long moh=0; char mou[48]="",me[160]="";
        if(haveM) sscanf(ml,"%127[^\t]\t%llx\t%47[^\t]\t%159[^\t\r\n]",mh,&moh,mou,me);
        int cmp = (haveS&&haveM)?strcmp(sh,mh):(haveS?-1:1);
        if(cmp==0){                       /* in both: map objhash, enum etag/present */
            if(sp){
                if(o) fprintf(o,"%016llx\t%s\t%s\t%d\n",moh,mh,se,1);
                if(need && strcmp(se,me)) fprintf(need,"%s\n",mh);   /* changed: wanted */
            }
            else if(mo) fprintf(mo,"d\t%016llx\t%s\t%s\n",moh,mh,me);  /* delta-DELETE: stage */
            haveS=s&&fgets(sl,sizeof sl,s)!=NULL; haveM=m&&fgets(ml,sizeof ml,m)!=NULL;
        } else if(cmp<0){                 /* server only */
            if(sp){                       /* a new present object: its UID is in its body */
                if(pend) fprintf(pend,"%s\t%s\n",sh,se);
                if(need) fprintf(need,"%s\n",sh);
            }                             /* else: delete of an object we never tracked -> ignore */
            haveS=s&&fgets(sl,sizeof sl,s)!=NULL;
        } else {                          /* map only: stage ('m'); present decided post-merge */
            if(mo) fprintf(mo,"m\t%016llx\t%s\t%s\n",moh,mh,me);
            haveM=m&&fgets(ml,sizeof ml,m)!=NULL;
        }
    }
    if(s){fclose(s);} if(m){fclose(m);}
    if(mo) fclose(mo);
    if(pend) fclose(pend);
    if(need) fclose(need);
    if(o) fclose(o);
}
static int resolvePending(const DavCtx*d,const char*coll,int incremental,int windowed){
    int unresolved=0;
    FILE*o=fopen(SV_IDX,"a"), *pend=fopen(SV_PEND,"r");
    char pl[512];
    while(pend && fgets(pl,sizeof pl,pend)){
        char sh[128]="",se[160]="";
        if(sscanf(pl,"%127[^\t]\t%159[^\t\r\n]",sh,se)<1) continue;
        char objuid[48]="";
        int got=fetchBody(d,coll,sh,g_objbuf,OBJ_FETCH_CAP);
        if(got>0 && got<OBJ_FETCH_CAP-1 && objuid_of(g_objbuf,objuid,sizeof objuid)==0){
            if(o) fprintf(o,"%016llx\t%s\t%s\t%d\n",(unsigned long long)uidHash(objuid),sh,se,1);
        } else {                          /* UID unreadable -> DEFER (never mint an href identity) */
            fprintf(stderr,"[sync] UID-resolve FAILED for server href=%s (got=%d) -- deferring, suppressing deletes this round\n",sh,got);
            unresolved=1;
        }
    }
    if(pend) fclose(pend);
    /* Flush staged rows. Three kinds of "is it really gone?":
     *   'm' map-only  : absent from the enumeration. In an incremental delta that
     *                   means UNCHANGED (present); in a window's listing it means
     *                   NOT LISTED (present=2: outside the window, or deleted --
     *                   the merge finds out which only if it matters); in a full
     *                   report it means DELETED.
     *   'd' delta-del : the delta explicitly reported this href deleted (only ever
     *                   happens incrementally) -> a real DELETE.
     * BUT if ANY object was deferred (unresolved) this round, no delete can be
     * trusted -- the "deleted" href may be the relocation source of the deferred
     * object -- so force every staged row present+unchanged and retry next sync. */
    FILE*mr=fopen(SV_MO,"r");
    if(mr && o){
        char l[512];
        while(fgets(l,sizeof l,mr)){
            char ty=0; unsigned long long moh=0; char mh[128]="",me[160]="";
            if(sscanf(l,"%c\t%llx\t%127[^\t]\t%159[^\t\r\n]",&ty,&moh,mh,me)>=3){
                int present = unresolved ? 1 : (ty=='m' ? (incremental ? 1 : windowed ? 2 : 0) : 0);
                fprintf(o,"%016llx\t%s\t%s\t%d\n",moh,mh, present?me:"", present);
            }
        }
    }
    if(mr) fclose(mr);
    if(o){fclose(o);}
    CHK("sv-idx-built"); sortFile(SV_IDX); CHK("sv-idx-sorted");
    return unresolved;
}

/* ---- merge-join rows: one parsed line from each sorted index file. A logical
 * record is the set of rows (at most one per source) sharing an objhash; keying
 * on the UID hash (not the href-derived uid) unifies a local record with its
 * server object across href relocations and lost map rows. ---- */
typedef struct { uint64_t oh; uint32_t uid; int idx,attr,len; uint64_t hash; int v; } LcRow;
typedef struct { uint64_t oh; uint32_t uid; char href[64],etag[160],objuid[48]; uint64_t hash; int v; } MpRow;
typedef struct { uint64_t oh; char href[128],etag[160]; int present; int v; } SvRow;

/* Each reader skips malformed lines and only reports v=0 at real EOF, so a stray
 * line never truncates a source stream mid-merge. A well-formed key line always
 * begins with a 16-hex-digit objhash. */
static void lcRead(FILE*f,LcRow*r){
    char ln[512]; r->v=0; if(!f) return;
    while(fgets(ln,sizeof ln,f)){
        unsigned long long oh,h; unsigned u;
        if(sscanf(ln,"%llx\t%u\t%d\t%d\t%d\t%llu",&oh,&u,&r->idx,&r->attr,&r->len,&h)>=6){ r->oh=oh;r->uid=u;r->hash=h;r->v=1; return; }
    }
}
static void mpRead(FILE*f,MpRow*r){
    char ln[1400]; r->v=0; if(!f) return;
    while(fgets(ln,sizeof ln,f)){
        unsigned long long oh,h; unsigned u; r->href[0]=r->etag[0]=r->objuid[0]=0;
        if(sscanf(ln,"%llx\t%u\t%63[^\t]\t%159[^\t]\t%llu\t%47[^\t\r\n]",&oh,&u,r->href,r->etag,&h,r->objuid)>=5){ r->oh=oh;r->uid=u;r->hash=h;r->v=1; return; }
    }
}
static void svRead(FILE*f,SvRow*r){
    char ln[512]; r->v=0; if(!f) return;
    while(fgets(ln,sizeof ln,f)){
        unsigned long long oh; r->href[0]=r->etag[0]=0; int p=0;
        if(sscanf(ln,"%llx\t%127[^\t]\t%159[^\t]\t%d",&oh,r->href,r->etag,&p)>=2){ r->oh=oh;r->present=p;r->v=1; return; }
    }
}

/* per-collection sink: the shared streamed output writer + this collection's
 * open map file + the category to stamp on records pulled from the server.   */
typedef struct { PdbW*w; FILE*mapf; int pullCat; } Sink;

static void keepBytes(Sink*k,uint32_t uid,uint8_t attr,const uint8_t*data,int len,
                      const char*href,const char*etag,uint64_t hash,const char*objuid){
    /* A dropped record here is not a lost row in a report -- it is a record
     * missing from the merged PDB, which the next sync reads as a local
     * deletion and pushes to the server. Never let that pass quietly. */
    if(pdbw_rec(k->w,uid,(uint8_t)(attr&~REC_ATTR_DIRTY&~REC_ATTR_DELETE),data,len)!=0){
        fprintf(stderr,"[sync] OUTPUT FULL at uid=%u -- the merged database cannot hold "
                       "this collection on this device\n",(unsigned)uid);
        s_too_big = 1;
        return;                       /* do not write a map row for a record we dropped */
    }
    if(k->mapf) fprintf(k->mapf,"%u\t%s\t%s\t%llu\t%s\n",
                        (unsigned)uid,href,etag,(unsigned long long)hash,objuid?objuid:"");
}

/* GET a server object, parse+pack into a local record, keep it (stamped with
 * the sink's pullCat category). The fetch buffer (g_objbuf) is generous on host;
 * the no-PSRAM device build keeps it small, so a photo-heavy vCard that overruns
 * it is skipped with a warning rather than silently truncated. */
static int keepFromServer(const DavCtx*d,const char*coll,int kind,Sink*k,
                          uint32_t uid,const char*name,const char*etag){
    int got=fetchBody(d,coll,name,g_objbuf,OBJ_FETCH_CAP);
    if(got<=0){ fprintf(stderr,"warning: could not fetch %s -- dropped\n",name); return -1; }
    if(got>=OBJ_FETCH_CAP-1){             /* hit the buffer limit => truncated */
        fprintf(stderr,"warning: %s exceeds %d bytes (large PHOTO?) -- dropped\n",name,OBJ_FETCH_CAP);
        return -1; }
    uint8_t tmp[PALM_REC_MAX];
    int l=parse_object(kind,g_objbuf,tmp,sizeof tmp);
    if(l<=0){ fprintf(stderr,"warning: could not parse %s -- dropped\n",name); return -1; }
    char ouid[48]=""; objuid_of(g_objbuf,ouid,sizeof ouid);   /* preserve the object's own UID */
    /* hash over the synth-UID body (UID-independent) so it matches loadRec's. */
    emit_object(kind,tmp,l,uid,g_body,BODY_CAP,NULL);
    keepBytes(k,uid,(uint8_t)k->pullCat,tmp,l,name,etag,fnv1a(g_body),ouid);
    return 0;
}

/* PUT a local record (bytes read lazily); ifmatch NULL = unconditional.
 * On 2xx: keep the record + write a FRESH map row (new href/etag/hash).
 * On failure (network or non-2xx HTTP): the record was NOT accepted by the
 * server, so keep it locally but DO NOT write the fresh row -- re-emit the OLD
 * map row (old* args; pass NULL/"" href when there is none, e.g. a brand-new
 * record) so the record stays mapped and dirty and is RETRIED next sync. This
 * stops a failed push from (a) being counted as a success and (b) poisoning the
 * map with a bad/empty etag (which would make the next sync treat the record as
 * server-deleted -> local data loss). Returns the HTTP status (<=0 -> -1); the
 * caller counts pushNew/pushMod only on a 2xx. */
/* `objuid` (or NULL): the UID this record already carries on the server (its
 * immutable id, from the map). When set, the pushed body preserves it and the
 * fresh/preserved map row records it; when NULL the record is brand-new and gets
 * the synth palm-<uid>@cyd. */
static int pushLocal(const DavCtx*d,const char*coll,int kind,Sink*k,
                     const S*s,const Loc*L,uint32_t uid,
                     const char*name,const char*ifmatch,
                     const char*oldHref,const char*oldEtag,uint64_t oldHash,
                     const char*objuid){
    if(locBytes(s,L,g_lrec,PALM_REC_MAX)!=L->len){
        fprintf(stderr,"[sync] lazy read failed for local uid=%u -- skipped\n",(unsigned)uid); return -1; }
    int bl=emit_object(kind,g_lrec,L->len,uid,g_body,BODY_CAP,objuid);
    if(bl<0) return -1;
    /* the UID actually stored in the object (override, else the synth default). */
    char synth[32]; snprintf(synth,sizeof synth,"palm-%u@cyd",(unsigned)uid);
    const char*storedUid = (objuid&&objuid[0]) ? objuid : synth;
    FILE*f=fopen(BODY_TMP,"wb"); if(!f) return -1; fwrite(g_body,1,strlen(g_body),f); fclose(f);
    char etag[160]=""; int st=0;
    put_guard(d,coll,name,kindCType(kind),BODY_TMP,ifmatch,etag,sizeof etag,&st);
    if(st>=200 && st<300){
        if(!etag[0]) dav_getetag(d,coll,name,etag,sizeof etag);   /* fallback */
        /* hash over the synth-UID body (UID-independent), matching loadRec. */
        emit_object(kind,g_lrec,L->len,uid,g_body,BODY_CAP,NULL);
        keepBytes(k,uid,L->attr,g_lrec,L->len,name,etag,fnv1a(g_body),storedUid); /* PDB + fresh map row */
        return st;
    }
    /* 412 = the UID already exists on the server at a DIFFERENT href (iCloud
     * enforces one-UID-per-collection). This is a conflict, not a transient
     * failure -- the caller resolves it per case (drop an orphan / pull the
     * server copy). Do NOT keep or re-map here, so the caller has a clean slate. */
    if(st==412) return st;
    /* transient failure (network / 5xx): keep the record locally + preserve the
     * OLD map row so it stays dirty and is retried next sync (never poison the
     * map with a bad etag). */
    fprintf(stderr,"[sync] push FAILED uid=%u (HTTP %d) -- kept local, will retry\n",(unsigned)uid,st);
    if(pdbw_rec(k->w,uid,(uint8_t)(L->attr&~REC_ATTR_DIRTY&~REC_ATTR_DELETE),g_lrec,L->len)!=0){
        fprintf(stderr,"[sync] OUTPUT FULL at uid=%u -- cannot keep the local record\n",(unsigned)uid);
        s_too_big = 1;
        return -1;
    }
    if(k->mapf && oldHref && oldHref[0])                                  /* preserve old mapping */
        fprintf(k->mapf,"%u\t%s\t%s\t%llu\t%s\n",(unsigned)uid,oldHref,oldEtag?oldEtag:"",
                (unsigned long long)oldHash,objuid?objuid:"");
    return st<=0 ? -1 : st;
}

/* Reconcile one collection by streaming merge-join. Builds the three UID-hash
 * index files (LC_IDX/MP_IDX/SV_IDX -- local / map / server), then walks them in
 * lockstep: for each distinct objhash it assembles the (loc,map,server) triple,
 * runs the reconcile matrix, does at most one DAV op, and appends the kept record
 * to the streamed writer *w plus a fresh row to the .tmp map. Only the current
 * objhash's rows are ever resident, so peak RAM during a DAV op is O(1) in the
 * record count. `rt`/`Ccoll` (or NULL) restrict local records to one routed
 * collection for category sync. */
/* 0 on success, -1 if the collection had to be abandoned before it changed
 * anything (the working buffers could not be retaken after the enumeration). */
static int sync_one(const DavCtx*d,S*s,const char*coll,const char*mapfile,
                    ConflictPolicy pol,PdbW*w,int pullCat,SyncStats*st,
                    const CatRoute*rt,const char*Ccoll){
    uint32_t maxuid=0; int incremental=0, windowed=0;
    s_too_big = 0; s_too_big_bytes = 0;                      /* per collection */
    bcReset();
    buildMapIdx(mapfile,s->token,sizeof s->token,&maxuid);   /* MP_IDX/MP_HREF/MP_PALM + token */
    if(s_too_big) return -1;
#ifdef ESP_PLATFORM
    /* Device: always FULL-enumerate. iCloud CalDAV PIM data is tiny, so the etag
     * list is cheap (streamed), and a persisted RFC 6578 sync-token can silently
     * orphan a record whose first pull failed -- the token advances past it and
     * incremental never re-reports it. A full reconcile every sync self-heals
     * that drift and re-tries any dropped pull. The host keeps the incremental
     * fast path (and its gates). Clearing the token here forces enumServer's full
     * PROPFIND/REPORT branch; the fresh token is not persisted (see below). */
    s->token[0]=0;
#endif
    bulk_free();          /* 16 KB back: the enumeration needs it more than we do */
    int enumOk = enumServer(d,coll,s->kind,s->token,s->newToken,sizeof s->newToken,
                            &incremental,&windowed);            /* SV_RAW */
    if(enumOk!=0){
        /* The server could not be enumerated (all reports/PROPFIND failed or were
         * truncated). SV_RAW is empty/partial and MUST NOT be read as "the server
         * deleted everything" -- that would wipe every mapped local record. Force
         * incremental semantics so resolveServer emits each mapped record as
         * PRESENT+unchanged (SCLEAN); with an empty SV_RAW there are no server
         * changes, so every record is kept as-is and the collection is a no-op
         * this round (it retries next sync). Also drop any stale new token. */
        fprintf(stderr,"[sync] %s: server enumeration failed -- keeping all local records (no deletes)\n",coll);
        incremental=1; windowed=0; s->newToken[0]=0;
    }
    resolveList();                                           /* SV_IDX (known), SV_PEND, NEED */
    if(s_too_big) return -1;
    prefetch(d,coll,s->kind);                                /* BC_DAT/BC_IDX: batched bodies */
    if(!need_objbuf()){
        fprintf(stderr,"[sync] %s: could not retake the %d-byte fetch buffer after the "
                       "enumeration -- abandoning this collection instead of running "
                       "without it\n", coll, (int)OBJ_FETCH_CAP);
        return -1;
    }
    int unresolved = resolvePending(d,coll,incremental,windowed); /* SV_IDX: new objects' UIDs */
    if(s_too_big) return -1;
    if(unresolved){
        /* Some server object's UID could not be read this round; resolveServer
         * already suppressed deletes. Don't advance the sync-token either, so the
         * deferred object is re-reported by the next incremental delta. */
        s->newToken[0]=0;
    }
    if(!need_body()){
        fprintf(stderr,"[sync] %s: could not retake the %d-byte emit buffer -- abandoning "
                       "this collection instead of running without it\n", coll, (int)BODY_CAP);
        return -1;
    }
    buildLcRaw(s->pdbpath,s->kind,rt,Ccoll,&maxuid);         /* LC_RAW */
    joinLcIdx();                                             /* LC_IDX */
    /* LAST GATE BEFORE THE MERGE. The three-way join below is a merge-join: it
     * assumes LC_IDX, MP_IDX and SV_IDX are sorted and walks them once. Handing
     * it an unsorted stream does not fail loudly -- it mis-pairs records, which
     * comes out as deletions and duplicates against the real account. */
    if(s_too_big) return -1;
    uint32_t seed=maxuid+1;

    char mtmp[512]; snprintf(mtmp,sizeof mtmp,"%s.tmp",mapfile);
    FILE*mapf=fopen(mtmp,"w");
#ifndef ESP_PLATFORM
    /* device never persists the sync-token (always full-enumerates -- see sync_one
     * top); the host keeps incremental, so it records the token for the next run. */
    if(mapf && s->newToken[0]) fprintf(mapf,"#synctoken\t%s\n",s->newToken);
#endif
    Sink K={ .w=w, .mapf=mapf, .pullCat=pullCat };
    Sink*k=&K;
    const char*ext = kindExt(s->kind); int kind=s->kind;

    /* ---- mass-delete guard --------------------------------------------------
     * "Present in the map, absent locally" means the user deleted it, and the
     * engine pushes that deletion to the server. That inference is only sound
     * while the local database is intact. If it isn't -- a PDB truncated by a
     * crash mid-write, then reseeded with demo rows as ABSENT on the next
     * boot -- every real record looks locally deleted, and a two-way sync
     * would erase the whole collection from the account. Writes are swapped
     * in whole (safefile.h), and the device keeps tombstones, so the guard is the
     * second line of defence rather than the only one; it stays, because a
     * card can still read short for reasons no write discipline prevents.
     *
     * So when most of the mapped records have vanished at once, treat it as the
     * local side being wrong rather than the user having deleted everything, and
     * PULL THE SERVER'S COPIES BACK DOWN.
     *
     * That last part is new (2026-09-22) and it is the whole point. Until now
     * the guard only DECLINED to delete: the mapped-but-locally-absent records
     * were skipped and nothing was written for them, so the device stayed empty
     * and the next sync reached exactly the same conclusion. The guard fired
     * forever, the server kept its records, the device kept none, and NEITHER
     * of this comment's two promises held -- nothing was restored, and a
     * genuine bulk delete never took effect either. It was a permanent
     * stalemate whose only symptom was a line on the UART. tests/massdel.c is
     * the gate that found it; there was none before, which is how a guard that
     * did half its job passed every suite for months.
     *
     * The trade this makes, stated plainly: on THIS device data_delete() drops
     * a record outright rather than leaving a Palm tombstone, so a real user
     * deletion and a bad card read are the same shape from in here. Restoring
     * therefore undoes a genuine bulk delete and the user has to delete again.
     * That is the right way round. Deleting half a collection through this UI
     * means visiting half a collection one record at a time; a truncated PDB
     * needs one bad write. And "my deleted events came back" is a bad hour,
     * while "my device is empty and every sync agrees" is the data loss the
     * guard exists to prevent. */
    int nLoc = countLines(LC_IDX), nMap = countLines(MP_IDX);
    int massGuard = (nMap >= MASSDEL_MIN) && (nLoc * 2 < nMap);
    int guardedDel = 0;       /* deletions held back (local tombstones)      */
    int guardedBack = 0;      /* records pulled back down from the server    */
    if(massGuard)
        fprintf(stderr,"[sync] MASS-DELETE GUARD for %s: %d local records against %d "
                       "mapped. Not pushing deletions this run -- if the local database "
                       "was lost, this sync restores it instead of erasing the server.\n",
                coll, nLoc, nMap);

    FILE*flc=fopen(LC_IDX,"r"),*fmp=fopen(MP_IDX,"r"),*fsv=fopen(SV_IDX,"r");
    LcRow lc; MpRow mp; SvRow sv;
    lcRead(flc,&lc); mpRead(fmp,&mp); svRead(fsv,&sv);

    CHK("merge-start");
    int chkIter = 0;
    while(lc.v||mp.v||sv.v){
        chk_n("merge", chkIter++);
        if(s_too_big) break;              /* output is full: stop, do not publish */
        uint64_t mn=~0ULL;
        if(lc.v&&lc.oh<mn)mn=lc.oh;
        if(mp.v&&mp.oh<mn)mn=mp.oh;
        if(sv.v&&sv.oh<mn)mn=sv.oh;
        int hasL   = lc.v&&lc.oh==mn;
        int hasMap = mp.v&&mp.oh==mn;
        /* consume ALL server rows at this objhash, preferring a present one: a
         * deleted (or not listed) row and its relocated replacement share a UID
         * hash, and only the present one is the live object. Rank: present (1),
         * then not listed (2), then deleted (0). */
        SvRow se; se.v=0;
        #define SVRANK(p) ((p)==1 ? 2 : (p)==2 ? 1 : 0)
        while(sv.v && sv.oh==mn){
            if(!se.v || SVRANK(sv.present) > SVRANK(se.present)) se=sv;
            svRead(fsv,&sv);
        }
        #undef SVRANK
        int hasSraw= se.v;
        int hasSrv = se.v && se.present==1;      /* a present=0 (deleted) srv row => no server object */
        int sOut   = se.v && se.present==2;      /* mapped, but not in the window's listing */

        uint32_t uid = hasL?lc.uid : (hasMap?mp.uid : nameToUid(se.href));
        Loc Lv; const Loc*L=NULL;
        if(hasL){ Lv.uid=lc.uid; Lv.attr=(uint8_t)lc.attr; Lv.pdbIdx=lc.idx; Lv.len=lc.len; Lv.hash=lc.hash; L=&Lv; }
        const char*mHref  = hasMap?mp.href:"";
        const char*mEtag  = hasMap?mp.etag:"";
        uint64_t   mHash  = hasMap?mp.hash:0;
        const char*mObjuid= hasMap?mp.objuid:NULL;
        const char*sName  = hasSrv?se.href:"";
        const char*sEtag  = hasSrv?se.etag:"";
        char pEtag[160];                          /* a probed object's etag */
        int ldel = L && (L->attr & REC_ATTR_DELETE);

        enum { LABSENT, LNEW, LMOD, LDEL, LCLEAN } lcs;
        if(!L)               lcs=LABSENT;
        else if(ldel)        lcs=LDEL;
        else if(!hasMap)     lcs=LNEW;
        else if(L->hash!=mHash) lcs=LMOD;
        else                 lcs=LCLEAN;

        /* SOUT: not in the window's listing -- outside the window, or deleted.
         * SKEEP: we asked and couldn't tell; leave everything as it is. */
        enum { SABSENT, SNEW, SMOD, SDEL, SCLEAN, SOUT, SKEEP } scs;
        if(!hasMap &&  hasSrv) scs=SNEW;
        else if(!hasMap)       scs=SABSENT;
        else if(!hasSrv)       scs= sOut ? SOUT : SDEL;
        else if(strcmp(sEtag,mEtag)) scs=SMOD;
        else                   scs=SCLEAN;

        /* Not listed, and the device has something to say about it (an edit, a
         * tombstone, or a record gone without one): which of the two it is now
         * matters, so ask the server about this one object. Still there -> it's
         * just outside the window, and the ordinary rules apply to it; gone ->
         * a server delete. No answer -> keep everything for next time. An
         * unchanged copy needs no question: either way it leaves the device. */
        if(scs==SOUT && (lcs==LMOD || lcs==LDEL || (lcs==LABSENT && !massGuard))){
            int ps=dav_probe(d,coll,mHref,pEtag,sizeof pEtag);
            if(ps==207 || ps==200){
                hasSrv=1; sName=mHref; sEtag=pEtag;
                scs = strcmp(pEtag,mEtag) ? SMOD : SCLEAN;
            } else if(ps==404 || ps==410) scs=SDEL;
            else scs=SKEEP;
        }

        char lname[64]; snprintf(lname,sizeof lname,"%u.%s",(unsigned)uid,ext);
        const char*srvName = hasSrv?sName:(hasMap?mHref:lname);

        /* relocation telemetry: the object is still one record (matched by UID)
         * but the server moved it to a new href -- the exact iCloud behavior we
         * need to observe on-device. Logged only when the hrefs actually differ. */
        if(hasMap && hasSrv && mHref[0] && sName[0] && strcmp(mHref,sName))
            SYNC_LOG("[sync] reloc uid=%u: map href=%s -> server href=%s (UID match)\n",
                    (unsigned)uid,mHref,sName);

        int conflict = (lcs==LMOD||lcs==LDEL||lcs==LNEW) && (scs==SMOD||scs==SNEW||scs==SDEL)
                       && !(lcs==LNEW&&scs==SABSENT) && !(lcs==LDEL&&scs==SDEL);

        if(scs==SOUT){
            /* Outside the window: off the device, and nothing is sent. (The only
             * local states left here are an unchanged copy, and a record gone
             * from the device while the mass-delete guard holds.) */
            if(L) st->pruned++;
        }
        else if(scs==SKEEP){
            /* Couldn't tell: keep the record exactly as it is -- flags and all,
             * so an edit or a tombstone is still pending -- and its old map row,
             * and decide at the next sync. */
            if(L){
                if(locBytes(s,L,g_lrec,PALM_REC_MAX)==L->len){
                    if(pdbw_rec(k->w,uid,L->attr,g_lrec,L->len)!=0){
                        fprintf(stderr,"[sync] OUTPUT FULL at uid=%u\n",(unsigned)uid); s_too_big=1; }
                } else fprintf(stderr,"[sync] lazy read failed for uid=%u\n",(unsigned)uid);
            }
            if(k->mapf && mHref[0])
                fprintf(k->mapf,"%u\t%s\t%s\t%llu\t%s\n",(unsigned)uid,mHref,mEtag,
                        (unsigned long long)mHash,mObjuid?mObjuid:"");
        }
        else if(conflict){
            st->conflicts++;
            int serverWins = (pol==POL_SERVER);
            int localWins  = (pol==POL_LOCAL);
            if(pol==POL_BOTH){
                if(scs==SDEL){ localWins=1; }
                else if(lcs==LDEL){ serverWins=1; }
                else {
                    if(scs!=SDEL) keepFromServer(d,coll,kind,k,uid,srvName,sEtag);
                    if(L && lcs!=LDEL){ uint32_t u2=seed++; char n2[64]; snprintf(n2,sizeof n2,"%u.%s",(unsigned)u2,ext);
                        pushLocal(d,coll,kind,k,s,L,u2,n2,NULL, NULL,NULL,0, NULL); }  /* fork = brand new */
                }
            } else if(serverWins){
                if(scs!=SDEL) keepFromServer(d,coll,kind,k,uid,srvName,sEtag);
            } else if(localWins){
                if(lcs==LDEL){ del_guard(d,coll,srvName,NULL); }
                else pushLocal(d,coll,kind,k,s,L,uid,srvName,NULL, mHref,mEtag,mHash, mObjuid);
            }
        }
        else if(lcs==LNEW && scs==SABSENT && s_hold && s_hold(uid, s_hold_ctx)){
            /* held back (the demo seed): carried through to the output exactly
             * as it was -- still dirty, still unmapped -- so it stays on the
             * device and is asked about again next time. Dropping it here would
             * lose it: the output PDB only holds what the merge writes. */
            if(locBytes(s,L,g_lrec,PALM_REC_MAX)==L->len){
                if(pdbw_rec(k->w,uid,L->attr,g_lrec,L->len)!=0){
                    fprintf(stderr,"[sync] OUTPUT FULL at held uid=%u\n",(unsigned)uid);
                    s_too_big = 1;
                } else st->held++;
            } else fprintf(stderr,"[sync] lazy read failed for held uid=%u\n",(unsigned)uid);
        }
        else if(lcs==LNEW && scs==SABSENT){
            int rc=pushLocal(d,coll,kind,k,s,L,uid,lname,NULL, NULL,NULL,0, NULL);
            if(rc>=200 && rc<300) st->pushNew++;
            else if(rc==412){
                /* the UID already lives on the server at another href (created
                 * elsewhere / orphaned map). DROP this local copy (not kept in the
                 * PDB); the server's object is pulled under its own href. */
                st->conflicts++;
                fprintf(stderr,"[sync] uid=%u dropped: dup UID already on server (server copy wins)\n",(unsigned)uid);
            }
        } else if(lcs==LMOD && scs==SCLEAN){
            int rc=pushLocal(d,coll,kind,k,s,L,uid,srvName,mEtag, mHref,mEtag,mHash, mObjuid);
            if(rc>=200 && rc<300) st->pushMod++;
            else if(rc==412){                    /* server changed under us -> take server copy */
                st->conflicts++;
                if(keepFromServer(d,coll,kind,k,uid,srvName,sEtag)==0) st->pullMod++;
            }
        } else if(lcs==LCLEAN && scs==SCLEAN){
            if(locBytes(s,L,g_lrec,PALM_REC_MAX)==L->len)
                keepBytes(k,uid,L->attr,g_lrec,L->len,srvName,mEtag,L->hash,mObjuid);
            else fprintf(stderr,"[sync] lazy read failed for clean uid=%u\n",(unsigned)uid);
            st->unchanged++;
        } else if(lcs==LCLEAN && scs==SMOD){
            if(keepFromServer(d,coll,kind,k,uid,srvName,sEtag)==0) st->pullMod++;
        } else if(lcs==LCLEAN && scs==SDEL){
            st->pullDel++;
        } else if(lcs==LDEL && scs==SCLEAN){
            /* A TOMBSTONE IS AN EXPLICIT DELETE, guard or no guard. The guard
             * exists because a record that is merely ABSENT could be a user's
             * delete or the card's loss; a record still in the database with
             * the delete bit set can only be the user's. So this pushes even
             * while the guard is holding back the absent ones below -- which
             * is what the device keeping tombstones until they sync buys. */
            if(!s_pull_only){ dav_delete(d,coll,srvName,mEtag); st->pushDel++; }
        } else if(lcs==LDEL && scs==SDEL){
            /* Gone from both sides. There is nothing to delete anywhere, so no
             * dav_delete is issued here -- and counting it as a pushDel (which
             * this did) reported a deletion that never left the device. A
             * tombstone is explicit, so the guard has nothing to say about it. */
            st->bothDel++;
        } else if(lcs==LABSENT && scs==SNEW){
            if(keepFromServer(d,coll,kind,k,uid,srvName,sEtag)==0) st->pullNew++;
        } else if(lcs==LABSENT && scs==SCLEAN){
            /* Mapped, still on the server, GONE from the local database with no
             * tombstone -- which is both "the user deleted it" and "the card
             * read short". Under the guard it is the second, so take the
             * server's copy back: this is the restore the guard promises, and
             * without it the record is simply dropped from the output PDB and
             * the device never heals. */
            if(massGuard){
                guardedDel++;
                if(keepFromServer(d,coll,kind,k,uid,srvName,sEtag)==0){
                    st->pullNew++; guardedBack++;
                }
            }
            else if(!s_pull_only){ dav_delete(d,coll,srvName,mEtag); st->pushDel++; }
        }

        progTick();                             /* one reconciled record */
        if(hasL)    lcRead(flc,&lc);
        if(hasMap)  mpRead(fmp,&mp);
        /* server rows at this objhash were already consumed above */
        (void)hasSraw;
    }
    CHK("merge-done");
    bcReset();
    if(guardedDel)
        fprintf(stderr,"[sync] MASS-DELETE GUARD held back %d deletion(s) for %s "
                       "and RESTORED %d record(s) from the server\n",
                guardedDel, coll, guardedBack);
    if(flc){fclose(flc);} if(fmp){fclose(fmp);} if(fsv){fclose(fsv);}

    if(s_too_big){
        /* The map describes a merge that will not be published, so publishing it
         * would leave the next run reconciling against a state that never existed. */
        if(mapf) fclose(mapf);
        remove(mtmp);
        return -1;
    }
    if(mapf){
        fclose(mapf);
        /* Publish the new map. FATFS rename() FAILS if the target exists (FR_EXIST),
         * so a plain rename over an existing map silently no-ops. Remove first. */
        remove(mapfile);
        if(rename(mtmp,mapfile)!=0)
            fprintf(stderr,"[sync] map publish FAILED for %s (rename errno=%d)\n",mapfile,errno);
    }
    return 0;
}

/* count records in a PDB (for the empty-overwrite safety check). */
static int countRecsCb(const PdbRec*r,int i,void*c){ (void)r;(void)i; (*(int*)c)++; return 0; }
static int countRecs(const char*pdb){ int n=0; pdb_read(pdb,countRecsCb,&n); return n; }

int sync_collection(const DavCtx*d,const char*localpdb,const char*outpdb,
                    const char*coll,int kind,const char*mapfile,
                    ConflictPolicy pol,SyncStats*st){
    if(over_budget()) return -5;
    if(!scratch_alloc()){ fprintf(stderr,"sync_collection: out of memory\n"); return -1; }
    S *s = g_state; memset(s,0,sizeof *s);
    s->kind=kind; snprintf(s->pdbpath,sizeof s->pdbpath,"%s",localpdb);
    s_fetch_batched = s_fetch_single = 0;
    static uint8_t ai[512]; int ailen=pdb_read_appinfo(localpdb,ai,sizeof ai); if(ailen<0)ailen=0;
    int nin = countRecs(localpdb);
    SYNC_LOG("[sync] read %s: recs=%d ailen=%d\n",localpdb,nin,ailen);
    SyncStats z={0}; if(!st) st=&z;

    PdbW *w = pdbw_begin(OUT_TMP);
    if(!w){ fprintf(stderr,"sync_collection: cannot open output temp\n"); return -3; }
    progReset(nin);
    /* -1: abandoned before the merge began, so nothing local was touched. Discard
     * the output and keep the PDB exactly as it was. */
    if(sync_one(d,s,coll,mapfile,pol,w,0,st,NULL,NULL) != 0){
        pdbw_abort(w);
        return s_too_big ? -6 : -1;
    }
    /* The transport gave up partway through, so the merged output is missing an
     * unknown number of server records. Publishing it would look like deletions.
     * Discard it and keep local untouched -- the next run reconciles from the
     * same map file, so nothing is lost but time. */
    if(dav_transport_down()){
        fprintf(stderr,"[sync] %s: transport down -- discarding partial merge, keeping local\n",coll);
        pdbw_abort(w); return -4;
    }
    int nrec = pdbw_count(w);
    SYNC_LOG("[sync] %s: out=%d push=%d/%d/%d pull=%d/%d/%d gone=%d\n",
            coll,nrec,st->pushNew,st->pushMod,st->pushDel,
            st->pullNew,st->pullMod,st->pullDel,st->bothDel);
    /* SAFETY: never overwrite a local PDB that HAD records with an empty result.
     * A read glitch, a parse failure, or an unexpectedly-empty server must not be
     * allowed to wipe the on-device data. Discard the streamed output, keep local. */
    if(nin > 0 && nrec == 0 && st->pruned < nin){
        fprintf(stderr,"[sync] REFUSED overwrite of %s (had %d recs) with 0 -- keeping local\n",outpdb,nin);
        pdbw_abort(w); return -2;
    }
    int rc = kindCommit(w,kind,outpdb, ailen?ai:NULL, ailen);
    return rc<0 ? -1 : nrec;
}

/* ---- category-routed multi-collection sync ---- */
static void sanitizeColl(const char*coll,char*out,int cap){
    int j=0; for(int i=0;coll[i]&&j<cap-1;i++){ char c=coll[i]; out[j++]=(c=='/'||c==':')?'_':c; } out[j]=0;
}

int sync_categorized(const DavCtx*d,const char*localpdb,const char*outpdb,
                     int kind,const CatRoute*rt,const char*mapdir,
                     ConflictPolicy pol,SyncStats*st){
    if(over_budget()) return -5;
    if(!scratch_alloc()){ fprintf(stderr,"sync_categorized: out of memory\n"); return -1; }
    static uint8_t ai[512]; int ailen=pdb_read_appinfo(localpdb,ai,sizeof ai); if(ailen<0)ailen=0;
    S *s = g_state; memset(s,0,sizeof *s);
    s->kind=kind; snprintf(s->pdbpath,sizeof s->pdbpath,"%s",localpdb);
    s_fetch_batched = s_fetch_single = 0;
    SyncStats z={0}; if(!st) st=&z;

    PdbW *w = pdbw_begin(OUT_TMP);
    if(!w){ fprintf(stderr,"sync_categorized: cannot open output temp\n"); return -3; }
    progReset(countRecs(localpdb));

    /* distinct destination collections + a representative category id each */
    const char* colls[CAT_COUNT+1]; int catOf[CAT_COUNT+1]; int nc=0;
    for(int c=0;c<CAT_COUNT;c++){
        const char*cl = rt->coll[c]?rt->coll[c]:rt->def; if(!cl||!cl[0]) continue;
        int found=0; for(int j=0;j<nc;j++) if(!strcmp(colls[j],cl)){ found=1; break; }
        if(!found){ colls[nc]=cl; catOf[nc]=c; nc++; }
    }
    if(rt->def && rt->def[0]){ int f=0; for(int j=0;j<nc;j++) if(!strcmp(colls[j],rt->def)){f=1;break;}
        if(!f){ colls[nc]=rt->def; catOf[nc]=0; nc++; } }

    for(int ci=0;ci<nc;ci++){
        const char*C=colls[ci];
        s->token[0]=0; s->newToken[0]=0;      /* rebuilt per collection by sync_one */
        char san[300], mapfile[400]; sanitizeColl(C,san,sizeof san);
        snprintf(mapfile,sizeof mapfile,"%s/%s.map",mapdir,san);
        if(sync_one(d,s,C,mapfile,pol,w,catOf[ci],st,rt,C) != 0){   /* rt/C filter local records to this coll */
            pdbw_abort(w); return s_too_big ? -6 : -1;
        }
    }
    if(dav_transport_down()){
        fprintf(stderr,"[sync] transport down -- discarding partial merge, keeping local\n");
        pdbw_abort(w); return -4;
    }
    int nrec = pdbw_count(w);
    int rc = kindCommit(w,kind,outpdb, ailen?ai:NULL, ailen);
    return rc<0 ? -1 : nrec;
}
