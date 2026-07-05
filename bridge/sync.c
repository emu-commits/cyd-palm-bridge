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
#include "palm.h"
#include "appinfo.h"
#include "sync.h"

/* Working-set sizing. Records are streamed, so what scales with MAXR is only the
 * per-record *index* (loc/map/srv/nodes), not the record bytes. The host proves
 * correctness with a generous MAXR; the ESP32 (no PSRAM, ~85 KB post-WiFi heap)
 * uses a smaller one. Define SYNC_DEVICE_SIZES to force the device sizing on a
 * host build (used by tests/bigsync.c to exercise the device cap on the desktop). */
#if defined(ESP_PLATFORM) || defined(SYNC_DEVICE_SIZES)
  /* Since the byte arenas are gone (records stream to/from disk), the resident
   * cost is ~0.6 KB per slot (loc+map+srv+2*node index). MAXR=96 keeps the whole
   * working set well under the old ~63 KB while lifting the per-collection cap
   * from 24 to 96 items. Raise further only after measuring heap on-device.
   * SYNC_DEVICE_SIZES forces this sizing on a host build (tests/bigsync.c) to
   * exercise the device cap on the desktop -- without moving the temp dir. */
  #define MAXR       96
  #define ARENA_CAP  (8*1024)          /* sync_pull only; loadRec no longer uses it */
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

/* Shared emit scratch, kept OFF the stack: the sync runs single-threaded and
 * never holds two bodies at once, so one 8 KB buffer serves every emit site. On
 * the device an 8 KB stack frame per record (loadRec runs for every record)
 * overflowed the task stack; this moves it to .bss so the stack stays small. */
static char g_body[8192];
/* one shared buffer for a single lazily-read local record (see locBytes). Also
 * off-stack for the same reason: the reconcile loop reads records one at a time. */
static uint8_t g_lrec[PALM_REC_MAX];

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

static int emit_object(int kind,const uint8_t*data,int len,uint32_t uid,char*out,int cap){
    if(kind==KIND_CAL){
        Appt a; if(ApptUnpack(data,len,&a)) return -1;
        char v[4096]; ical_emit(v,sizeof v,&a,uid);
        char vt[1200]; int vl=(a.hasTime)?ical_vtimezone(vt,sizeof vt):0; if(vl<0)vl=0; if(!vl)vt[0]=0;
        return snprintf(out,cap,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//CYD-Palm-Bridge//EN\r\n%s%sEND:VCALENDAR\r\n",vt,v);
    } else if(kind==KIND_TODO){
        Todo t; if(ToDoUnpack(data,len,&t)) return -1;
        char v[2048]; vtodo_emit(v,sizeof v,&t,uid);
        return snprintf(out,cap,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//CYD-Palm-Bridge//EN\r\n%sEND:VCALENDAR\r\n",v);
    } else {
        Addr a; if(AddrUnpack(data,len,&a)) return -1;
        return vcard_emit(out,cap,&a,uid);
    }
}
/* server object bytes -> packed Palm record (inverse of emit_object). len or -1. */
static int parse_object(int kind,const char*obj,uint8_t*out,int cap){
    if(kind==KIND_CAL){ Appt a; if(ical_parse(obj,&a)) return -1; return ApptPack(out,cap,&a); }
    if(kind==KIND_TODO){ Todo t; if(vtodo_parse(obj,&t)) return -1; return ToDoPack(out,cap,&t); }
    Addr a; if(vcard_parse(obj,&a)) return -1; return AddrPack(out,cap,&a);
}

static int pushRec(const PdbRec*r,int i,void*ctx){
    (void)i; PushCtx*p=ctx;
    int bl=emit_object(p->kind,r->data,r->len,r->uniqueID,g_body,sizeof g_body);
    if(bl<0) return 0;
    FILE*f=fopen(BODY_TMP,"wb"); fwrite(g_body,1,strlen(g_body),f); fclose(f);
    char name[64]; snprintf(name,sizeof name,"%u.%s",(unsigned)r->uniqueID,kindExt(p->kind));
    char etag[160]=""; int st=0;
    dav_put(p->d,p->coll,name,kindCType(p->kind),BODY_TMP,NULL,etag,sizeof etag,&st);
    fprintf(p->map,"%d\t%u\t%s\t%s\n",p->kind,(unsigned)r->uniqueID,name,etag);
    p->n++;
    return 0;
}

int sync_push(const DavCtx*d,const char*pdbpath,const char*coll,int kind){
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

/* ---- state loaded for reconciliation ---- */
typedef struct {                       /* local records (index only; bytes read lazily) */
    uint32_t uid; uint8_t attr; int pdbIdx; int len; uint64_t hash;
} Loc;
typedef struct {                       /* persisted map rows */
    uint32_t uid; char href[64]; char etag[160]; uint64_t hash;
} Map;
typedef struct {                       /* server objects (from PROPFIND / sync REPORT) */
    char name[128]; char etag[160]; uint32_t uid; int present;
} Srv;

typedef struct {
    int kind;
    char pdbpath[256];                 /* source PDB for lazy local record reads */
    Loc loc[MAXR]; int nloc;
    Map map[MAXR]; int nmap;
    Srv srv[MAXR]; int nsrv;
    char token[1408];                  /* RFC 6578 sync-token from last run   */
    char newToken[1408];               /* token to persist after this run     */
} S;

/* read one local record's bytes on demand (returns len, or -1). */
static int locBytes(const S*s,const Loc*L,uint8_t*buf,int cap){
    return pdb_read_one(s->pdbpath,L->pdbIdx,buf,cap,NULL,NULL);
}

static int loadRec(const PdbRec*r,int i,void*ctx){
    S*s=ctx;
    if(s->nloc>=MAXR){ fprintf(stderr,"[sync] loc index full at %d recs -- rest not synced\n",s->nloc); return 1; }
    Loc*L=&s->loc[s->nloc];
    L->uid=r->uniqueID; L->attr=r->attr; L->len=r->len; L->pdbIdx=i;
    if(emit_object(s->kind,r->data,r->len,r->uniqueID,g_body,sizeof g_body)<0) L->hash=0;
    else L->hash=fnv1a(g_body);
    s->nloc++;
    return 0;
}
static void loadMap(S*s,const char*mapfile){
    FILE*f=fopen(mapfile,"r"); if(!f) return;
    char line[1400];
    while(fgets(line,sizeof line,f)){
        if(!strncmp(line,"#synctoken\t",11)){
            char *t=line+11; size_t l=strlen(t); while(l&&(t[l-1]=='\n'||t[l-1]=='\r'))t[--l]=0;
            snprintf(s->token,sizeof s->token,"%s",t); continue;
        }
        Map m; memset(&m,0,sizeof m); unsigned long long h=0; unsigned mu=0;
        if(sscanf(line,"%u\t%63[^\t]\t%159[^\t]\t%llu",&mu,m.href,m.etag,&h)>=3){
            m.uid=(uint32_t)mu; m.hash=(uint64_t)h; if(s->nmap<MAXR) s->map[s->nmap++]=m;
        }
    }
    fclose(f);
}
/* find/add a server slot by object name */
static Srv* srvSlot(S*s,const char*name){
    for(int i=0;i<s->nsrv;i++) if(!strcmp(s->srv[i].name,name)) return &s->srv[i];
    if(s->nsrv>=MAXR) return NULL;
    Srv*v=&s->srv[s->nsrv++]; memset(v,0,sizeof*v);
    snprintf(v->name,sizeof v->name,"%s",name); v->uid=nameToUid(name); return v;
}
static void srvCb(const char*name,const char*etag,void*ctx){   /* full PROPFIND listing */
    Srv*v=srvSlot((S*)ctx,name); if(!v) return;
    snprintf(v->etag,sizeof v->etag,"%s",etag); v->present=1;
}
static void reportCb(const char*name,const char*etag,int deleted,void*ctx){ /* sync REPORT delta */
    Srv*v=srvSlot((S*)ctx,name); if(!v) return;
    if(deleted){ v->present=0; }
    else { snprintf(v->etag,sizeof v->etag,"%s",etag); v->present=1; }
}

/* Build current server state into s->srv[], preferring RFC 6578 sync-collection.
 * Returns via s->newToken the token to persist (empty if unsupported).          */
static void buildServer(S*s,const DavCtx*d,const char*coll){
    s->newToken[0]=0;
    if(s->token[0]){
        /* incremental: seed baseline from map (unchanged), then apply the delta */
        for(int i=0;i<s->nmap;i++){ Srv*v=srvSlot(s,s->map[i].href);
            if(v){ snprintf(v->etag,sizeof v->etag,"%s",s->map[i].etag); v->present=1; } }
        int rc=dav_sync_report(d,coll,s->token,reportCb,s,s->newToken,sizeof s->newToken);
        if(rc==0) return;                    /* delta applied                     */
        /* token invalid or unsupported -> reset and fall through to full */
        s->nsrv=0; s->token[0]=0; s->newToken[0]=0;
    }
    /* full sync: try REPORT with empty token (also yields a fresh token) */
    int rc=dav_sync_report(d,coll,"",reportCb,s,s->newToken,sizeof s->newToken);
    if(rc==0) return;
    /* server doesn't support sync-collection: plain PROPFIND, no token */
    s->nsrv=0; s->newToken[0]=0;
    dav_list(d,coll,srvCb,s);
}

/* ---- reconciliation node: indices into s->loc/map/srv for one uid (no string
 * copies -- keeps the node table tiny so MAXR can be large on the device). ---- */
typedef struct {
    uint32_t uid;
    int li;   /* index into s->loc, or -1 */
    int mi;   /* index into s->map, or -1 */
    int si;   /* index into s->srv, or -1 */
} Node;

static Node* nodeFor(Node*nodes,int*nn,uint32_t uid){
    for(int i=0;i<*nn;i++) if(nodes[i].uid==uid) return &nodes[i];
    Node*n=&nodes[(*nn)++]; n->uid=uid; n->li=n->mi=n->si=-1; return n;
}

/* per-collection sink: the shared streamed output writer + this collection's
 * open map file + the category to stamp on records pulled from the server.   */
typedef struct { PdbW*w; FILE*mapf; int pullCat; } Sink;

static void keepBytes(Sink*k,uint32_t uid,uint8_t attr,const uint8_t*data,int len,
                      const char*href,const char*etag,uint64_t hash){
    pdbw_rec(k->w,uid,(uint8_t)(attr&~REC_ATTR_DIRTY&~REC_ATTR_DELETE),data,len);
    if(k->mapf) fprintf(k->mapf,"%u\t%s\t%s\t%llu\n",
                        (unsigned)uid,href,etag,(unsigned long long)hash);
}

/* GET a server object, parse+pack into a local record, keep it (stamped with
 * the sink's pullCat category). */
/* fetch buffer for one server object. iCloud contacts can embed a base64 PHOTO
 * that pushes the raw vCard well past 16 KB; too small a buffer truncates the
 * object (no END:VCARD) and it silently fails to parse. Generous on host; the
 * device build (no PSRAM) overrides this small. */
#ifndef OBJ_FETCH_CAP
#ifdef ESP_PLATFORM
#define OBJ_FETCH_CAP (8*1024)       /* no PSRAM: photo-heavy vCards get skipped+warned */
#else
#define OBJ_FETCH_CAP (256*1024)
#endif
#endif
static int keepFromServer(const DavCtx*d,const char*coll,int kind,Sink*k,
                          uint32_t uid,const char*name,const char*etag){
    static char obj[OBJ_FETCH_CAP];       /* static, not stack (too big for stack) */
    int got=dav_get(d,coll,name,obj,sizeof obj);
    if(got<=0){ fprintf(stderr,"warning: could not fetch %s -- dropped\n",name); return -1; }
    if(got>=OBJ_FETCH_CAP-1){             /* hit the buffer limit => truncated */
        fprintf(stderr,"warning: %s exceeds %d bytes (large PHOTO?) -- dropped\n",name,OBJ_FETCH_CAP);
        return -1; }
    uint8_t tmp[PALM_REC_MAX];
    int l=parse_object(kind,obj,tmp,sizeof tmp);
    if(l<=0){ fprintf(stderr,"warning: could not parse %s -- dropped\n",name); return -1; }
    emit_object(kind,tmp,l,uid,g_body,sizeof g_body);
    keepBytes(k,uid,(uint8_t)k->pullCat,tmp,l,name,etag,fnv1a(g_body));
    return 0;
}

/* PUT a local record (bytes read lazily); ifmatch NULL = unconditional. keep it. */
static int pushLocal(const DavCtx*d,const char*coll,int kind,Sink*k,
                     const S*s,const Loc*L,uint32_t uid,
                     const char*name,const char*ifmatch){
    if(locBytes(s,L,g_lrec,sizeof g_lrec)!=L->len){
        fprintf(stderr,"[sync] lazy read failed for local uid=%u -- skipped\n",(unsigned)uid); return -1; }
    int bl=emit_object(kind,g_lrec,L->len,uid,g_body,sizeof g_body);
    if(bl<0) return -1;
    FILE*f=fopen(BODY_TMP,"wb"); if(!f) return -1; fwrite(g_body,1,strlen(g_body),f); fclose(f);
    char etag[160]=""; int st=0;
    dav_put(d,coll,name,kindCType(kind),BODY_TMP,ifmatch,etag,sizeof etag,&st);
    if(!etag[0]) dav_getetag(d,coll,name,etag,sizeof etag);   /* fallback */
    keepBytes(k,uid,L->attr,g_lrec,L->len,name,etag,fnv1a(g_body));
    return st;
}

/* Reconcile one preloaded subset (s->loc filled, s->kind set) against one
 * collection; append kept records to the streamed writer *w; rewrite this
 * collection's map atomically (write to .tmp, rename on completion).         */
static void sync_one(const DavCtx*d,S*s,const char*coll,const char*mapfile,
                     ConflictPolicy pol,PdbW*w,int pullCat,SyncStats*st){
    loadMap(s,mapfile);
    buildServer(s,d,coll);

    /* heap (not static BSS) so this working set is freed after the sync and the
     * memory returns to the UI in interactive mode. */
    Node *nodes = calloc((size_t)MAXR*2, sizeof *nodes);
    if(!nodes){ fprintf(stderr,"sync_one: out of memory\n"); return; }
    int nn=0;
    uint32_t seed=1;
    for(int i=0;i<s->nloc;i++){ Node*n=nodeFor(nodes,&nn,s->loc[i].uid); n->li=i; if(s->loc[i].uid>=seed)seed=s->loc[i].uid+1; }
    for(int i=0;i<s->nmap;i++){ Node*n=nodeFor(nodes,&nn,s->map[i].uid); n->mi=i;
        if(s->map[i].uid>=seed)seed=s->map[i].uid+1; }
    for(int i=0;i<s->nsrv;i++){ if(!s->srv[i].present) continue;
        Node*n=nodeFor(nodes,&nn,s->srv[i].uid); n->si=i;
        if(s->srv[i].uid>=seed)seed=s->srv[i].uid+1; }

    /* open the map for streaming (atomic: write .tmp, rename over mapfile at end).
     * loadMap already read the old contents into s->map/s->token above.        */
    char mtmp[512]; snprintf(mtmp,sizeof mtmp,"%s.tmp",mapfile);
    FILE*mapf=fopen(mtmp,"w");
    if(mapf && s->newToken[0]) fprintf(mapf,"#synctoken\t%s\n",s->newToken);
    Sink K={ .w=w, .mapf=mapf, .pullCat=pullCat };
    Sink*k=&K;
    const char*ext = kindExt(s->kind); int kind=s->kind;

    for(int i=0;i<nn;i++){
        Node*n=&nodes[i];
        const Loc*L   = n->li>=0 ? &s->loc[n->li] : NULL;
        int hasMap    = n->mi>=0;
        int hasSrv    = n->si>=0;
        const char*mHref = hasMap ? s->map[n->mi].href : "";
        const char*mEtag = hasMap ? s->map[n->mi].etag : "";
        uint64_t   mHash = hasMap ? s->map[n->mi].hash : 0;
        const char*sName = hasSrv ? s->srv[n->si].name : "";
        const char*sEtag = hasSrv ? s->srv[n->si].etag : "";
        int ldel = L && (L->attr & REC_ATTR_DELETE);

        enum { LABSENT, LNEW, LMOD, LDEL, LCLEAN } lc;
        if(!L)               lc=LABSENT;
        else if(ldel)        lc=LDEL;
        else if(!hasMap)     lc=LNEW;
        else if(L->hash!=mHash) lc=LMOD;
        else                 lc=LCLEAN;

        enum { SABSENT, SNEW, SMOD, SDEL, SCLEAN } sc;
        if(!hasMap &&  hasSrv) sc=SNEW;
        else if(!hasMap)       sc=SABSENT;
        else if(!hasSrv)       sc=SDEL;
        else if(strcmp(sEtag,mEtag)) sc=SMOD;
        else                   sc=SCLEAN;

        char lname[64]; snprintf(lname,sizeof lname,"%u.%s",(unsigned)n->uid,ext);
        const char*srvName = hasSrv?sName:(hasMap?mHref:lname);

        int conflict = (lc==LMOD||lc==LDEL||lc==LNEW) && (sc==SMOD||sc==SNEW||sc==SDEL)
                       && !(lc==LNEW&&sc==SABSENT) && !(lc==LDEL&&sc==SDEL);

        if(conflict){
            st->conflicts++;
            int serverWins = (pol==POL_SERVER);
            int localWins  = (pol==POL_LOCAL);
            if(pol==POL_BOTH){
                if(sc==SDEL){ localWins=1; }
                else if(lc==LDEL){ serverWins=1; }
                else {
                    if(sc!=SDEL) keepFromServer(d,coll,kind,k,n->uid,srvName,sEtag);
                    if(L && lc!=LDEL){ uint32_t u2=seed++; char n2[64]; snprintf(n2,sizeof n2,"%u.%s",(unsigned)u2,ext);
                        pushLocal(d,coll,kind,k,s,L,u2,n2,NULL); }
                    continue;
                }
            }
            if(serverWins){
                if(sc!=SDEL) keepFromServer(d,coll,kind,k,n->uid,srvName,sEtag);
            } else if(localWins){
                if(lc==LDEL){ dav_delete(d,coll,srvName,NULL); }
                else pushLocal(d,coll,kind,k,s,L,n->uid,lname,NULL);
            }
            continue;
        }

        if(lc==LNEW && sc==SABSENT){
            pushLocal(d,coll,kind,k,s,L,n->uid,lname,NULL); st->pushNew++;
        } else if(lc==LMOD && sc==SCLEAN){
            pushLocal(d,coll,kind,k,s,L,n->uid,srvName,mEtag); st->pushMod++;
        } else if(lc==LCLEAN && sc==SCLEAN){
            if(locBytes(s,L,g_lrec,sizeof g_lrec)==L->len)
                keepBytes(k,n->uid,L->attr,g_lrec,L->len,srvName,mEtag,L->hash);
            else fprintf(stderr,"[sync] lazy read failed for clean uid=%u\n",(unsigned)n->uid);
            st->unchanged++;
        } else if(lc==LCLEAN && sc==SMOD){
            if(keepFromServer(d,coll,kind,k,n->uid,srvName,sEtag)==0) st->pullMod++;
        } else if(lc==LCLEAN && sc==SDEL){
            st->pullDel++;
        } else if(lc==LDEL && sc==SCLEAN){
            dav_delete(d,coll,srvName,mEtag); st->pushDel++;
        } else if(lc==LDEL && sc==SDEL){
            st->pushDel++;
        } else if(lc==LABSENT && sc==SNEW){
            if(keepFromServer(d,coll,kind,k,n->uid,srvName,sEtag)==0) st->pullNew++;
        } else if(lc==LABSENT && sc==SCLEAN){
            dav_delete(d,coll,srvName,mEtag); st->pushDel++;
        }
    }

    if(mapf){ fclose(mapf); rename(mtmp,mapfile); }   /* atomically publish the new map */
    free(nodes);
}

int sync_collection(const DavCtx*d,const char*localpdb,const char*outpdb,
                    const char*coll,int kind,const char*mapfile,
                    ConflictPolicy pol,SyncStats*st){
    S *s = calloc(1,sizeof *s);
    if(!s){ fprintf(stderr,"sync_collection: out of memory\n"); return -1; }
    s->kind=kind; snprintf(s->pdbpath,sizeof s->pdbpath,"%s",localpdb);
    static uint8_t ai[512]; int ailen=pdb_read_appinfo(localpdb,ai,sizeof ai); if(ailen<0)ailen=0;
    int nin = pdb_read(localpdb,loadRec,s);
    fprintf(stderr,"[sync] read %s: pdb_read=%d nloc=%d ailen=%d\n",localpdb,nin,s->nloc,ailen);
    SyncStats z={0}; if(!st) st=&z;

    PdbW *w = pdbw_begin(OUT_TMP);
    if(!w){ free(s); fprintf(stderr,"sync_collection: cannot open output temp\n"); return -1; }
    sync_one(d,s,coll,mapfile,pol,w,0,st);
    int nrec = pdbw_count(w);
    fprintf(stderr,"[sync] %s: out=%d nmap=%d nsrv=%d push=%d/%d/%d pull=%d/%d/%d\n",
            coll,nrec,s->nmap,s->nsrv,st->pushNew,st->pushMod,st->pushDel,
            st->pullNew,st->pullMod,st->pullDel);
    /* SAFETY: never overwrite a local PDB that HAD records with an empty result.
     * A read glitch, a parse failure, or an unexpectedly-empty server must not be
     * allowed to wipe the on-device data. Discard the streamed output, keep local. */
    if(nin > 0 && nrec == 0){
        fprintf(stderr,"[sync] REFUSED overwrite of %s (had %d recs) with 0 -- keeping local\n",outpdb,nin);
        pdbw_abort(w); free(s); return -2;
    }
    int rc = kindCommit(w,kind,outpdb, ailen?ai:NULL, ailen);
    free(s);
    return rc<0 ? -1 : nrec;
}

/* ---- category-routed multi-collection sync ---- */
static void sanitizeColl(const char*coll,char*out,int cap){
    int j=0; for(int i=0;coll[i]&&j<cap-1;i++){ char c=coll[i]; out[j++]=(c=='/'||c==':')?'_':c; } out[j]=0;
}

int sync_categorized(const DavCtx*d,const char*localpdb,const char*outpdb,
                     int kind,const CatRoute*rt,const char*mapdir,
                     ConflictPolicy pol,SyncStats*st){
    static uint8_t ai[512]; int ailen=pdb_read_appinfo(localpdb,ai,sizeof ai); if(ailen<0)ailen=0;
    S *all = calloc(1,sizeof *all); S *sub = calloc(1,sizeof *sub);
    if(!all || !sub){ free(all); free(sub); fprintf(stderr,"sync_categorized: out of memory\n"); return -1; }
    all->kind=kind; snprintf(all->pdbpath,sizeof all->pdbpath,"%s",localpdb);
    pdb_read(localpdb,loadRec,all);
    SyncStats z={0}; if(!st) st=&z;

    PdbW *w = pdbw_begin(OUT_TMP);
    if(!w){ free(all); free(sub); fprintf(stderr,"sync_categorized: cannot open output temp\n"); return -1; }

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
        memset(sub,0,sizeof *sub); sub->kind=kind;
        snprintf(sub->pdbpath,sizeof sub->pdbpath,"%s",localpdb);   /* lazy reads hit the source */
        for(int i=0;i<all->nloc;i++){
            int cat = all->loc[i].attr & REC_ATTR_CAT;
            const char*dest = rt->coll[cat]?rt->coll[cat]:rt->def;
            if(!dest || strcmp(dest,C)) continue;
            if(sub->nloc>=MAXR) break;
            sub->loc[sub->nloc++]=all->loc[i];    /* copy index only; pdbIdx still valid */
        }
        char san[300], mapfile[400]; sanitizeColl(C,san,sizeof san);
        snprintf(mapfile,sizeof mapfile,"%s/%s.map",mapdir,san);
        sync_one(d,sub,C,mapfile,pol,w,catOf[ci],st);
    }
    int nrec = pdbw_count(w);
    int rc = kindCommit(w,kind,outpdb, ailen?ai:NULL, ailen);
    free(all); free(sub);
    return rc<0 ? -1 : nrec;
}
