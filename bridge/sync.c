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
 * the merged PDB and the refreshed map. Records are streamed; only a bounded
 * working set is held (host proof uses static arenas; the device build caps
 * these far lower -- real Palm PIM DBs are a few hundred records of a few KB).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "palm.h"
#include "appinfo.h"
#include "sync.h"

/* Working-set sizing. The host proves correctness with big static arenas; the
 * ESP32 (no PSRAM, ~300KB SRAM) needs them small. Records average a few hundred
 * bytes, so the record arena is sized in bytes (ARENA_CAP), decoupled from the
 * index count (MAXR). Override on the device build; host sizes are unchanged. */
#ifdef ESP_PLATFORM
  #define MAXR       48
  #define ARENA_CAP  (16*1024)         /* ~340B/record avg over MAXR; no-PSRAM budget */
  #define NAMEL_MAX  64
  #define STATE_DIR  "/sdcard/state"   /* device cwd is "/"; temps live on SD */
#else
  #define MAXR       256
  #define ARENA_CAP  (MAXR*PALM_REC_MAX)
  #define NAMEL_MAX  256
  #define STATE_DIR  "state"
#endif
#define BODY_TMP STATE_DIR "/.body"    /* PUT body staged here before upload */

/* ---- kind helpers ---- */
static const char* kindExt(int k){ return k==KIND_CARD?"vcf":"ics"; }
static const char* kindCType(int k){ return k==KIND_CARD?"text/vcard; charset=utf-8":"text/calendar; charset=utf-8"; }
static int kindWrite(int k,const char*path,const uint8_t*ai,int ailen,const PdbRec*recs,int nrec){
    if(k==KIND_CAL)  return pdb_write_ai(path,"DatebookDB",0x44415441,0x64617465,ai,ailen,recs,nrec);
    if(k==KIND_TODO) return pdb_write_ai(path,"ToDoDB",   0x44415441,0x746F646F,ai,ailen,recs,nrec);
    return                pdb_write_ai(path,"AddressDB", 0x44415441,0x61646472,ai,ailen,recs,nrec);
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
    char body[8192]; int bl=emit_object(p->kind,r->data,r->len,r->uniqueID,body,sizeof body);
    if(bl<0) return 0;
    FILE*f=fopen(BODY_TMP,"wb"); fwrite(body,1,strlen(body),f); fclose(f);
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
typedef struct {                       /* local records (from PDB) */
    uint32_t uid; uint8_t attr; int len; int off; uint64_t hash;
} Loc;
typedef struct {                       /* persisted map rows */
    uint32_t uid; char href[64]; char etag[160]; uint64_t hash;
} Map;
typedef struct {                       /* server objects (from PROPFIND / sync REPORT) */
    char name[128]; char etag[160]; uint32_t uid; int present;
} Srv;

typedef struct {
    int kind;
    Loc loc[MAXR]; int nloc; uint8_t locArena[ARENA_CAP]; int locUsed;
    Map map[MAXR]; int nmap;
    Srv srv[MAXR]; int nsrv;
    char token[1408];                  /* RFC 6578 sync-token from last run   */
    char newToken[1408];               /* token to persist after this run     */
} S;

static int loadRec(const PdbRec*r,int i,void*ctx){
    (void)i; S*s=ctx; if(s->nloc>=MAXR) return 1;
    Loc*L=&s->loc[s->nloc];
    L->uid=r->uniqueID; L->attr=r->attr; L->len=r->len; L->off=s->locUsed;
    memcpy(s->locArena+s->locUsed,r->data,r->len); s->locUsed+=r->len;
    char body[8192]; if(emit_object(s->kind,r->data,r->len,r->uniqueID,body,sizeof body)<0) L->hash=0;
    else L->hash=fnv1a(body);
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

/* ---- reconciliation node: everything known about one uid ---- */
typedef struct {
    uint32_t uid;
    int li;                          /* local index or -1                 */
    int hasMap; char mHref[64]; char mEtag[160]; uint64_t mHash;
    int hasSrv; char sName[128]; char sEtag[160];
} Node;

static Node* nodeFor(Node*nodes,int*nn,uint32_t uid){
    for(int i=0;i<*nn;i++) if(nodes[i].uid==uid) return &nodes[i];
    Node*n=&nodes[(*nn)++]; memset(n,0,sizeof *n); n->uid=uid; n->li=-1; return n;
}

/* ---- output accumulator: kept records -> merged PDB ---- */
typedef struct {
    PdbRec rec[MAXR]; int nrec; uint8_t arena[ARENA_CAP]; int used;
} Out;
/* per-collection sink: shared record output + this collection's map rows +
 * the category to stamp on records pulled from the server.                  */
typedef struct { Out*o; Map map[MAXR]; int nmap; int pullCat; } Sink;

static void keepBytes(Sink*k,uint32_t uid,uint8_t attr,const uint8_t*data,int len,
                      const char*href,const char*etag,uint64_t hash){
    Out*o=k->o;
    if(o->nrec>=MAXR || o->used+len>(int)sizeof o->arena) return;
    uint8_t*dst=o->arena+o->used; memcpy(dst,data,len);
    o->rec[o->nrec]=(PdbRec){ .attr=(uint8_t)(attr&~REC_ATTR_DIRTY&~REC_ATTR_DELETE),
                              .uniqueID=uid,.data=dst,.len=len };
    o->used+=len; o->nrec++;
    Map*m=&k->map[k->nmap++]; m->uid=uid; snprintf(m->href,sizeof m->href,"%s",href);
    snprintf(m->etag,sizeof m->etag,"%s",etag); m->hash=hash;
}

/* GET a server object, parse+pack into a local record, keep it (stamped with
 * the sink's pullCat category). */
/* fetch buffer for one server object. iCloud contacts can embed a base64 PHOTO
 * that pushes the raw vCard well past 16 KB; too small a buffer truncates the
 * object (no END:VCARD) and it silently fails to parse. Generous on host; the
 * device build (B3, no PSRAM) overrides this small. */
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
    char body[8192]; emit_object(kind,tmp,l,uid,body,sizeof body);
    keepBytes(k,uid,(uint8_t)k->pullCat,tmp,l,name,etag,fnv1a(body));
    return 0;
}

/* PUT a local record; ifmatch NULL = unconditional. keep it locally. */
static int pushLocal(const DavCtx*d,const char*coll,int kind,Sink*k,
                     const Loc*L,const uint8_t*arena,uint32_t uid,
                     const char*name,const char*ifmatch){
    char body[8192]; int bl=emit_object(kind,arena+L->off,L->len,uid,body,sizeof body);
    if(bl<0) return -1;
    FILE*f=fopen(BODY_TMP,"wb"); fwrite(body,1,strlen(body),f); fclose(f);
    char etag[160]=""; int st=0;
    dav_put(d,coll,name,kindCType(kind),BODY_TMP,ifmatch,etag,sizeof etag,&st);
    if(!etag[0]) dav_getetag(d,coll,name,etag,sizeof etag);   /* fallback */
    keepBytes(k,uid,L->attr,arena+L->off,L->len,name,etag,fnv1a(body));
    return st;
}

/* Reconcile one preloaded subset (s->loc filled, s->kind set) against one
 * collection; append kept records to *o; rewrite this collection's map.      */
static void sync_one(const DavCtx*d,S*s,const char*coll,const char*mapfile,
                     ConflictPolicy pol,Out*o,int pullCat,SyncStats*st){
    loadMap(s,mapfile);
    buildServer(s,d,coll);

    /* heap (not static BSS) so this working set is freed after the sync and the
     * ~memory returns to the UI in interactive mode. */
    Node *nodes = calloc((size_t)MAXR*2, sizeof *nodes);
    Sink *k = calloc(1, sizeof *k);
    if(!nodes || !k){ free(nodes); free(k); fprintf(stderr,"sync_one: out of memory\n"); return; }
    int nn=0;
    uint32_t seed=1;
    for(int i=0;i<s->nloc;i++){ Node*n=nodeFor(nodes,&nn,s->loc[i].uid); n->li=i; if(s->loc[i].uid>=seed)seed=s->loc[i].uid+1; }
    for(int i=0;i<s->nmap;i++){ Node*n=nodeFor(nodes,&nn,s->map[i].uid); n->hasMap=1;
        snprintf(n->mHref,sizeof n->mHref,"%s",s->map[i].href);
        snprintf(n->mEtag,sizeof n->mEtag,"%s",s->map[i].etag); n->mHash=s->map[i].hash;
        if(s->map[i].uid>=seed)seed=s->map[i].uid+1; }
    for(int i=0;i<s->nsrv;i++){ if(!s->srv[i].present) continue;
        Node*n=nodeFor(nodes,&nn,s->srv[i].uid); n->hasSrv=1;
        snprintf(n->sName,sizeof n->sName,"%s",s->srv[i].name);
        snprintf(n->sEtag,sizeof n->sEtag,"%s",s->srv[i].etag);
        if(s->srv[i].uid>=seed)seed=s->srv[i].uid+1; }

    k->o=o; k->pullCat=pullCat;   /* nmap already 0 from calloc */
    const char*ext = kindExt(s->kind); int kind=s->kind;

    for(int i=0;i<nn;i++){
        Node*n=&nodes[i];
        const Loc*L = n->li>=0 ? &s->loc[n->li] : NULL;
        int ldel = L && (L->attr & REC_ATTR_DELETE);

        enum { LABSENT, LNEW, LMOD, LDEL, LCLEAN } lc;
        if(!L)               lc=LABSENT;
        else if(ldel)        lc=LDEL;
        else if(!n->hasMap)  lc=LNEW;
        else if(L->hash!=n->mHash) lc=LMOD;
        else                 lc=LCLEAN;

        enum { SABSENT, SNEW, SMOD, SDEL, SCLEAN } sc;
        if(!n->hasMap &&  n->hasSrv) sc=SNEW;
        else if(!n->hasMap)          sc=SABSENT;
        else if(!n->hasSrv)          sc=SDEL;
        else if(strcmp(n->sEtag,n->mEtag)) sc=SMOD;
        else                         sc=SCLEAN;

        char lname[64]; snprintf(lname,sizeof lname,"%u.%s",(unsigned)n->uid,ext);
        const char*srvName = n->hasSrv?n->sName:(n->hasMap?n->mHref:lname);

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
                    if(sc!=SDEL) keepFromServer(d,coll,kind,k,n->uid,srvName,n->sEtag);
                    if(L && lc!=LDEL){ uint32_t u2=seed++; char n2[64]; snprintf(n2,sizeof n2,"%u.%s",(unsigned)u2,ext);
                        pushLocal(d,coll,kind,k,L,s->locArena,u2,n2,NULL); }
                    continue;
                }
            }
            if(serverWins){
                if(sc!=SDEL) keepFromServer(d,coll,kind,k,n->uid,srvName,n->sEtag);
            } else if(localWins){
                if(lc==LDEL){ dav_delete(d,coll,srvName,NULL); }
                else pushLocal(d,coll,kind,k,L,s->locArena,n->uid,lname,NULL);
            }
            continue;
        }

        if(lc==LNEW && sc==SABSENT){
            pushLocal(d,coll,kind,k,L,s->locArena,n->uid,lname,NULL); st->pushNew++;
        } else if(lc==LMOD && sc==SCLEAN){
            pushLocal(d,coll,kind,k,L,s->locArena,n->uid,srvName,n->mEtag); st->pushMod++;
        } else if(lc==LCLEAN && sc==SCLEAN){
            keepBytes(k,n->uid,L->attr,s->locArena+L->off,L->len,srvName,n->mEtag,L->hash); st->unchanged++;
        } else if(lc==LCLEAN && sc==SMOD){
            if(keepFromServer(d,coll,kind,k,n->uid,srvName,n->sEtag)==0) st->pullMod++;
        } else if(lc==LCLEAN && sc==SDEL){
            st->pullDel++;
        } else if(lc==LDEL && sc==SCLEAN){
            dav_delete(d,coll,srvName,n->mEtag); st->pushDel++;
        } else if(lc==LDEL && sc==SDEL){
            st->pushDel++;
        } else if(lc==LABSENT && sc==SNEW){
            if(keepFromServer(d,coll,kind,k,n->uid,srvName,n->sEtag)==0) st->pullNew++;
        } else if(lc==LABSENT && sc==SCLEAN){
            dav_delete(d,coll,srvName,n->mEtag); st->pushDel++;
        }
    }

    /* rewrite this collection's map (token header + one row per kept record) */
    FILE*mf=fopen(mapfile,"w");
    if(mf){
        if(s->newToken[0]) fprintf(mf,"#synctoken\t%s\n",s->newToken);
        for(int i=0;i<k->nmap;i++) fprintf(mf,"%u\t%s\t%s\t%llu\n",
                (unsigned)k->map[i].uid,k->map[i].href,k->map[i].etag,(unsigned long long)k->map[i].hash);
        fclose(mf); }
    free(nodes); free(k);
}

static void sortByUid(Out*o){
    for(int i=1;i<o->nrec;i++){ PdbRec k=o->rec[i]; int j=i-1;
        while(j>=0 && o->rec[j].uniqueID>k.uniqueID){ o->rec[j+1]=o->rec[j]; j--; } o->rec[j+1]=k; }
}

int sync_collection(const DavCtx*d,const char*localpdb,const char*outpdb,
                    const char*coll,int kind,const char*mapfile,
                    ConflictPolicy pol,SyncStats*st){
    S *s = calloc(1,sizeof *s); Out *o = calloc(1,sizeof *o);
    if(!s || !o){ free(s); free(o); fprintf(stderr,"sync_collection: out of memory\n"); return -1; }
    s->kind=kind;
    static uint8_t ai[512]; int ailen=pdb_read_appinfo(localpdb,ai,sizeof ai); if(ailen<0)ailen=0;
    pdb_read(localpdb,loadRec,s);
    SyncStats z={0}; if(!st) st=&z;
    sync_one(d,s,coll,mapfile,pol,o,0,st);
    sortByUid(o);
    kindWrite(kind,outpdb, ailen?ai:NULL, ailen, o->rec,o->nrec);
    int n=o->nrec; free(s); free(o); return n;
}

/* ---- category-routed multi-collection sync ---- */
static void sanitizeColl(const char*coll,char*out,int cap){
    int j=0; for(int i=0;coll[i]&&j<cap-1;i++){ char c=coll[i]; out[j++]=(c=='/'||c==':')?'_':c; } out[j]=0;
}

int sync_categorized(const DavCtx*d,const char*localpdb,const char*outpdb,
                     int kind,const CatRoute*rt,const char*mapdir,
                     ConflictPolicy pol,SyncStats*st){
    static uint8_t ai[512]; int ailen=pdb_read_appinfo(localpdb,ai,sizeof ai); if(ailen<0)ailen=0;
    S *all = calloc(1,sizeof *all); Out *o = calloc(1,sizeof *o); S *sub = calloc(1,sizeof *sub);
    if(!all || !o || !sub){ free(all); free(o); free(sub); fprintf(stderr,"sync_categorized: out of memory\n"); return -1; }
    all->kind=kind;
    pdb_read(localpdb,loadRec,all);
    SyncStats z={0}; if(!st) st=&z;

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
        for(int i=0;i<all->nloc;i++){
            int cat = all->loc[i].attr & REC_ATTR_CAT;
            const char*dest = rt->coll[cat]?rt->coll[cat]:rt->def;
            if(!dest || strcmp(dest,C)) continue;
            if(sub->nloc>=MAXR || sub->locUsed+all->loc[i].len>(int)sizeof sub->locArena) break;
            Loc*L=&sub->loc[sub->nloc]; *L=all->loc[i]; L->off=sub->locUsed;
            memcpy(sub->locArena+sub->locUsed, all->locArena+all->loc[i].off, all->loc[i].len);
            sub->locUsed+=all->loc[i].len; sub->nloc++;
        }
        char san[300], mapfile[400]; sanitizeColl(C,san,sizeof san);
        snprintf(mapfile,sizeof mapfile,"%s/%s.map",mapdir,san);
        sync_one(d,sub,C,mapfile,pol,o,catOf[ci],st);
    }
    sortByUid(o);
    kindWrite(kind,outpdb, ailen?ai:NULL, ailen, o->rec,o->nrec);
    int n=o->nrec; free(all); free(o); free(sub); return n;
}
