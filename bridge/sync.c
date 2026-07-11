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

/* Working-set sizing. NOTE: as of the streaming reconcile (sync_collection /
 * sync_one below), MAXR NO LONGER caps a synced collection -- reconciliation is a
 * disk-backed merge-join whose resident cost during a DAV call is O(1). MAXR now
 * bounds ONLY the legacy full-sync primitives sync_push/sync_pull (used by the
 * bridge_cli one-shot commands, not by HotSync). SYNC_DEVICE_SIZES still forces
 * device sizing on a host build so tests/bigsync.c can prove the streaming engine
 * scales past the old 24-record wall on the desktop. */
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
    int bl=emit_object(p->kind,r->data,r->len,r->uniqueID,g_body,sizeof g_body,NULL);
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
 * relocated href or a lost map row no longer splits one record into two. */
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
static char g_objbuf[OBJ_FETCH_CAP];

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
/* sort a line file in place by first field. Loads it whole into RAM -- the only
 * O(N) step, and it runs with no handshake live so it gets the full free block.
 * (For collections beyond what free heap can sort, an external merge sort is the
 * next increment; Palm-scale data is well within an in-RAM sort.) */
static void sortFile(const char*path){
    /* About to malloc the whole file for an in-memory qsort. On the no-PSRAM
     * device this heap must not fight the ~40 KB TLS working set, so release any
     * live keep-alive connection first (no-op on the host). The next DAV call
     * transparently reconnects; this keeps "TLS never resident during a sort". */
    dav_disconnect();
    FILE*f=fopen(path,"rb"); if(!f) return;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0){ fclose(f); return; }
    char*buf=malloc((size_t)sz+1);
    if(!buf){ fclose(f); fprintf(stderr,"[sync] sortFile OOM (%ld bytes) for %s\n",sz,path); return; }
    size_t got=fread(buf,1,(size_t)sz,f); buf[got]=0; fclose(f);
    int cap=64,n=0; char**lines=malloc((size_t)cap*sizeof*lines);
    if(!lines){ free(buf); return; }
    for(char*p=buf;*p;){
        if(n>=cap){ cap*=2; char**t=realloc(lines,(size_t)cap*sizeof*lines); if(!t){ free(lines); free(buf); return; } lines=t; }
        lines[n++]=p; char*nl=strchr(p,'\n'); if(!nl) break; *nl=0; p=nl+1;
    }
    qsort(lines,n,sizeof*lines,cmpLine);
    FILE*o=fopen(path,"wb");
    if(o){ for(int i=0;i<n;i++) fprintf(o,"%s\n",lines[i]); fclose(o); }
    free(lines); free(buf);
}

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
    sortFile(MP_IDX); sortFile(MP_HREF); sortFile(MP_PALM);
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
    if(emit_object(b->kind,r->data,r->len,r->uniqueID,g_body,sizeof g_body,NULL)>=0) hash=fnv1a(g_body);
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
    sortFile(LC_RAW);
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
    sortFile(LC_IDX);
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
static int enumServer(const DavCtx*d,const char*coll,const char*token,
                      char*newtok,int tokcap,int*incremental){
    *incremental=0; newtok[0]=0;
    RawEnum re; re.f=fopen(SV_RAW,"w");
    int done=0, ok=0;
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
/* Resolve every server object to an objhash and write SV_IDX (key=objhash):
 * "objhash href etag present". Joins SV_RAW (key href) with MP_HREF (key href):
 *   both        -> objhash from the map (no GET); etag/present from enumeration.
 *   map only    -> unchanged (incremental) present w/ map etag, else deleted.
 *   server only -> a new object: GET it once to read its UID -> objhash.
 *
 * Returns 1 if ANY server-only object could NOT be UID-resolved (its GET failed,
 * truncated on the small no-PSRAM fetch buffer, or was unparseable), else 0.
 * Such an object is DEFERRED, not force-identified: the old code fell back to
 * uidHash(href), which minted a divergent identity for what is really an already
 * mapped record -- so the mapped copy looked server-deleted (spurious delete) AND
 * the object looked brand-new (phantom pull). That split is the on-device
 * duplication seen against iCloud (relocated photo-vCards overflow the 8 KB
 * buffer). Instead we skip the object this round and tell the caller to SUPPRESS
 * DELETES (so a transient GET failure can never delete the mapped local record),
 * and it retries cleanly next sync. Map-only rows are therefore staged to SV_MO
 * and only flushed as deletes once we know the enumeration was fully resolved. */
static int resolveServer(const DavCtx*d,const char*coll,int incremental){
    sortFile(SV_RAW);                         /* merge-join needs it keyed (by href) */
    FILE*s=fopen(SV_RAW,"r"),*m=fopen(MP_HREF,"r"),*o=fopen(SV_IDX,"w");
    FILE*mo=fopen(SV_MO,"w");                  /* map-only rows, present decided after merge */
    int unresolved=0;
    char sl[512],ml[512];
    int haveS=s&&fgets(sl,sizeof sl,s), haveM=m&&fgets(ml,sizeof ml,m);
    while(haveS||haveM){
        char sh[128]="",se[160]=""; int sp=0;
        if(haveS) sscanf(sl,"%127[^\t]\t%159[^\t]\t%d",sh,se,&sp);
        char mh[128]=""; unsigned long long moh=0; char mou[48]="",me[160]="";
        if(haveM) sscanf(ml,"%127[^\t]\t%llx\t%47[^\t]\t%159[^\t\r\n]",mh,&moh,mou,me);
        int cmp = (haveS&&haveM)?strcmp(sh,mh):(haveS?-1:1);
        if(cmp==0){                       /* in both: map objhash, enum etag/present */
            if(sp){ if(o) fprintf(o,"%016llx\t%s\t%s\t%d\n",moh,mh,se,1); }
            else if(mo) fprintf(mo,"d\t%016llx\t%s\t%s\n",moh,mh,me);  /* delta-DELETE: stage */
            haveS=s&&fgets(sl,sizeof sl,s)!=NULL; haveM=m&&fgets(ml,sizeof ml,m)!=NULL;
        } else if(cmp<0){                 /* server only */
            if(sp){                       /* a new present object -> GET for its UID */
                char objuid[48]=""; uint64_t oh;
                int got=dav_get(d,coll,sh,g_objbuf,sizeof g_objbuf);
                if(got>0 && got<OBJ_FETCH_CAP-1 && objuid_of(g_objbuf,objuid,sizeof objuid)==0){
                    oh=uidHash(objuid);
                    if(o) fprintf(o,"%016llx\t%s\t%s\t%d\n",(unsigned long long)oh,sh,se,1);
                } else {                  /* UID unreadable -> DEFER (never mint an href identity) */
                    fprintf(stderr,"[sync] UID-resolve FAILED for server href=%s (got=%d) -- deferring, suppressing deletes this round\n",sh,got);
                    unresolved=1;
                }
            }                             /* else: delete of an object we never tracked -> ignore */
            haveS=s&&fgets(sl,sizeof sl,s)!=NULL;
        } else {                          /* map only: stage ('m'); present decided post-merge */
            if(mo) fprintf(mo,"m\t%016llx\t%s\t%s\n",moh,mh,me);
            haveM=m&&fgets(ml,sizeof ml,m)!=NULL;
        }
    }
    if(s){fclose(s);} if(m){fclose(m);}
    if(mo) fclose(mo);
    /* Flush staged rows. Two kinds, different "is it really gone?" semantics:
     *   'm' map-only  : absent from the enumeration. In an incremental delta that
     *                   means UNCHANGED (present); in a full report it means DELETED.
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
                int present = unresolved ? 1 : (ty=='m' ? incremental : 0);
                fprintf(o,"%016llx\t%s\t%s\t%d\n",moh,mh, present?me:"", present);
            }
        }
    }
    if(mr) fclose(mr);
    if(o){fclose(o);}
    sortFile(SV_IDX);
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
    pdbw_rec(k->w,uid,(uint8_t)(attr&~REC_ATTR_DIRTY&~REC_ATTR_DELETE),data,len);
    if(k->mapf) fprintf(k->mapf,"%u\t%s\t%s\t%llu\t%s\n",
                        (unsigned)uid,href,etag,(unsigned long long)hash,objuid?objuid:"");
}

/* GET a server object, parse+pack into a local record, keep it (stamped with
 * the sink's pullCat category). The fetch buffer (g_objbuf) is generous on host;
 * the no-PSRAM device build keeps it small, so a photo-heavy vCard that overruns
 * it is skipped with a warning rather than silently truncated. */
static int keepFromServer(const DavCtx*d,const char*coll,int kind,Sink*k,
                          uint32_t uid,const char*name,const char*etag){
    int got=dav_get(d,coll,name,g_objbuf,sizeof g_objbuf);
    if(got<=0){ fprintf(stderr,"warning: could not fetch %s -- dropped\n",name); return -1; }
    if(got>=OBJ_FETCH_CAP-1){             /* hit the buffer limit => truncated */
        fprintf(stderr,"warning: %s exceeds %d bytes (large PHOTO?) -- dropped\n",name,OBJ_FETCH_CAP);
        return -1; }
    uint8_t tmp[PALM_REC_MAX];
    int l=parse_object(kind,g_objbuf,tmp,sizeof tmp);
    if(l<=0){ fprintf(stderr,"warning: could not parse %s -- dropped\n",name); return -1; }
    char ouid[48]=""; objuid_of(g_objbuf,ouid,sizeof ouid);   /* preserve the object's own UID */
    /* hash over the synth-UID body (UID-independent) so it matches loadRec's. */
    emit_object(kind,tmp,l,uid,g_body,sizeof g_body,NULL);
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
    if(locBytes(s,L,g_lrec,sizeof g_lrec)!=L->len){
        fprintf(stderr,"[sync] lazy read failed for local uid=%u -- skipped\n",(unsigned)uid); return -1; }
    int bl=emit_object(kind,g_lrec,L->len,uid,g_body,sizeof g_body,objuid);
    if(bl<0) return -1;
    /* the UID actually stored in the object (override, else the synth default). */
    char synth[32]; snprintf(synth,sizeof synth,"palm-%u@cyd",(unsigned)uid);
    const char*storedUid = (objuid&&objuid[0]) ? objuid : synth;
    FILE*f=fopen(BODY_TMP,"wb"); if(!f) return -1; fwrite(g_body,1,strlen(g_body),f); fclose(f);
    char etag[160]=""; int st=0;
    dav_put(d,coll,name,kindCType(kind),BODY_TMP,ifmatch,etag,sizeof etag,&st);
    if(st>=200 && st<300){
        if(!etag[0]) dav_getetag(d,coll,name,etag,sizeof etag);   /* fallback */
        /* hash over the synth-UID body (UID-independent), matching loadRec. */
        emit_object(kind,g_lrec,L->len,uid,g_body,sizeof g_body,NULL);
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
    pdbw_rec(k->w,uid,(uint8_t)(L->attr&~REC_ATTR_DIRTY&~REC_ATTR_DELETE),g_lrec,L->len); /* keep local */
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
static void sync_one(const DavCtx*d,S*s,const char*coll,const char*mapfile,
                     ConflictPolicy pol,PdbW*w,int pullCat,SyncStats*st,
                     const CatRoute*rt,const char*Ccoll){
    uint32_t maxuid=0; int incremental=0;
    buildMapIdx(mapfile,s->token,sizeof s->token,&maxuid);   /* MP_IDX/MP_HREF/MP_PALM + token */
    int enumOk = enumServer(d,coll,s->token,s->newToken,sizeof s->newToken,&incremental); /* SV_RAW */
    if(enumOk!=0){
        /* The server could not be enumerated (all reports/PROPFIND failed or were
         * truncated). SV_RAW is empty/partial and MUST NOT be read as "the server
         * deleted everything" -- that would wipe every mapped local record. Force
         * incremental semantics so resolveServer emits each mapped record as
         * PRESENT+unchanged (SCLEAN); with an empty SV_RAW there are no server
         * changes, so every record is kept as-is and the collection is a no-op
         * this round (it retries next sync). Also drop any stale new token. */
        fprintf(stderr,"[sync] %s: server enumeration failed -- keeping all local records (no deletes)\n",coll);
        incremental=1; s->newToken[0]=0;
    }
    int unresolved = resolveServer(d,coll,incremental);      /* SV_IDX (GETs new objects) */
    if(unresolved){
        /* Some server object's UID could not be read this round; resolveServer
         * already suppressed deletes. Don't advance the sync-token either, so the
         * deferred object is re-reported by the next incremental delta. */
        s->newToken[0]=0;
    }
    buildLcRaw(s->pdbpath,s->kind,rt,Ccoll,&maxuid);         /* LC_RAW */
    joinLcIdx();                                             /* LC_IDX */
    uint32_t seed=maxuid+1;

    char mtmp[512]; snprintf(mtmp,sizeof mtmp,"%s.tmp",mapfile);
    FILE*mapf=fopen(mtmp,"w");
    if(mapf && s->newToken[0]) fprintf(mapf,"#synctoken\t%s\n",s->newToken);
    Sink K={ .w=w, .mapf=mapf, .pullCat=pullCat };
    Sink*k=&K;
    const char*ext = kindExt(s->kind); int kind=s->kind;

    FILE*flc=fopen(LC_IDX,"r"),*fmp=fopen(MP_IDX,"r"),*fsv=fopen(SV_IDX,"r");
    LcRow lc; MpRow mp; SvRow sv;
    lcRead(flc,&lc); mpRead(fmp,&mp); svRead(fsv,&sv);

    while(lc.v||mp.v||sv.v){
        uint64_t mn=~0ULL;
        if(lc.v&&lc.oh<mn)mn=lc.oh;
        if(mp.v&&mp.oh<mn)mn=mp.oh;
        if(sv.v&&sv.oh<mn)mn=sv.oh;
        int hasL   = lc.v&&lc.oh==mn;
        int hasMap = mp.v&&mp.oh==mn;
        /* consume ALL server rows at this objhash, preferring a present one: a
         * deleted row and its relocated replacement share a UID hash, and only
         * the present one is the live object. */
        SvRow se; se.v=0;
        while(sv.v && sv.oh==mn){
            if(!se.v || (sv.present && !se.present)) se=sv;
            svRead(fsv,&sv);
        }
        int hasSraw= se.v;
        int hasSrv = se.v && se.present;         /* a present=0 (deleted) srv row => no server object */

        uint32_t uid = hasL?lc.uid : (hasMap?mp.uid : nameToUid(se.href));
        Loc Lv; const Loc*L=NULL;
        if(hasL){ Lv.uid=lc.uid; Lv.attr=(uint8_t)lc.attr; Lv.pdbIdx=lc.idx; Lv.len=lc.len; Lv.hash=lc.hash; L=&Lv; }
        const char*mHref  = hasMap?mp.href:"";
        const char*mEtag  = hasMap?mp.etag:"";
        uint64_t   mHash  = hasMap?mp.hash:0;
        const char*mObjuid= hasMap?mp.objuid:NULL;
        const char*sName  = hasSrv?se.href:"";
        const char*sEtag  = hasSrv?se.etag:"";
        int ldel = L && (L->attr & REC_ATTR_DELETE);

        enum { LABSENT, LNEW, LMOD, LDEL, LCLEAN } lcs;
        if(!L)               lcs=LABSENT;
        else if(ldel)        lcs=LDEL;
        else if(!hasMap)     lcs=LNEW;
        else if(L->hash!=mHash) lcs=LMOD;
        else                 lcs=LCLEAN;

        enum { SABSENT, SNEW, SMOD, SDEL, SCLEAN } scs;
        if(!hasMap &&  hasSrv) scs=SNEW;
        else if(!hasMap)       scs=SABSENT;
        else if(!hasSrv)       scs=SDEL;
        else if(strcmp(sEtag,mEtag)) scs=SMOD;
        else                   scs=SCLEAN;

        char lname[64]; snprintf(lname,sizeof lname,"%u.%s",(unsigned)uid,ext);
        const char*srvName = hasSrv?sName:(hasMap?mHref:lname);

        /* relocation telemetry: the object is still one record (matched by UID)
         * but the server moved it to a new href -- the exact iCloud behavior we
         * need to observe on-device. Logged only when the hrefs actually differ. */
        if(hasMap && hasSrv && mHref[0] && sName[0] && strcmp(mHref,sName))
            fprintf(stderr,"[sync] reloc uid=%u: map href=%s -> server href=%s (UID match)\n",
                    (unsigned)uid,mHref,sName);

        int conflict = (lcs==LMOD||lcs==LDEL||lcs==LNEW) && (scs==SMOD||scs==SNEW||scs==SDEL)
                       && !(lcs==LNEW&&scs==SABSENT) && !(lcs==LDEL&&scs==SDEL);

        if(conflict){
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
                if(lcs==LDEL){ dav_delete(d,coll,srvName,NULL); }
                else pushLocal(d,coll,kind,k,s,L,uid,srvName,NULL, mHref,mEtag,mHash, mObjuid);
            }
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
            if(locBytes(s,L,g_lrec,sizeof g_lrec)==L->len)
                keepBytes(k,uid,L->attr,g_lrec,L->len,srvName,mEtag,L->hash,mObjuid);
            else fprintf(stderr,"[sync] lazy read failed for clean uid=%u\n",(unsigned)uid);
            st->unchanged++;
        } else if(lcs==LCLEAN && scs==SMOD){
            if(keepFromServer(d,coll,kind,k,uid,srvName,sEtag)==0) st->pullMod++;
        } else if(lcs==LCLEAN && scs==SDEL){
            st->pullDel++;
        } else if(lcs==LDEL && scs==SCLEAN){
            dav_delete(d,coll,srvName,mEtag); st->pushDel++;
        } else if(lcs==LDEL && scs==SDEL){
            st->pushDel++;
        } else if(lcs==LABSENT && scs==SNEW){
            if(keepFromServer(d,coll,kind,k,uid,srvName,sEtag)==0) st->pullNew++;
        } else if(lcs==LABSENT && scs==SCLEAN){
            dav_delete(d,coll,srvName,mEtag); st->pushDel++;
        }

        progTick();                             /* one reconciled record */
        if(hasL)    lcRead(flc,&lc);
        if(hasMap)  mpRead(fmp,&mp);
        /* server rows at this objhash were already consumed above */
        (void)hasSraw;
    }
    if(flc){fclose(flc);} if(fmp){fclose(fmp);} if(fsv){fclose(fsv);}

    if(mapf){
        fclose(mapf);
        /* Publish the new map. FATFS rename() FAILS if the target exists (FR_EXIST),
         * so a plain rename over an existing map silently no-ops. Remove first. */
        remove(mapfile);
        if(rename(mtmp,mapfile)!=0)
            fprintf(stderr,"[sync] map publish FAILED for %s (rename errno=%d)\n",mapfile,errno);
    }
}

/* count records in a PDB (for the empty-overwrite safety check). */
static int countRecsCb(const PdbRec*r,int i,void*c){ (void)r;(void)i; (*(int*)c)++; return 0; }
static int countRecs(const char*pdb){ int n=0; pdb_read(pdb,countRecsCb,&n); return n; }

int sync_collection(const DavCtx*d,const char*localpdb,const char*outpdb,
                    const char*coll,int kind,const char*mapfile,
                    ConflictPolicy pol,SyncStats*st){
    S *s = calloc(1,sizeof *s);
    if(!s){ fprintf(stderr,"sync_collection: out of memory\n"); return -1; }
    s->kind=kind; snprintf(s->pdbpath,sizeof s->pdbpath,"%s",localpdb);
    static uint8_t ai[512]; int ailen=pdb_read_appinfo(localpdb,ai,sizeof ai); if(ailen<0)ailen=0;
    int nin = countRecs(localpdb);
    fprintf(stderr,"[sync] read %s: recs=%d ailen=%d\n",localpdb,nin,ailen);
    SyncStats z={0}; if(!st) st=&z;

    PdbW *w = pdbw_begin(OUT_TMP);
    if(!w){ free(s); fprintf(stderr,"sync_collection: cannot open output temp\n"); return -1; }
    progReset(nin);
    sync_one(d,s,coll,mapfile,pol,w,0,st,NULL,NULL);
    int nrec = pdbw_count(w);
    fprintf(stderr,"[sync] %s: out=%d push=%d/%d/%d pull=%d/%d/%d\n",
            coll,nrec,st->pushNew,st->pushMod,st->pushDel,
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
    S *s = calloc(1,sizeof *s);
    if(!s){ fprintf(stderr,"sync_categorized: out of memory\n"); return -1; }
    s->kind=kind; snprintf(s->pdbpath,sizeof s->pdbpath,"%s",localpdb);
    SyncStats z={0}; if(!st) st=&z;

    PdbW *w = pdbw_begin(OUT_TMP);
    if(!w){ free(s); fprintf(stderr,"sync_categorized: cannot open output temp\n"); return -1; }
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
        sync_one(d,s,C,mapfile,pol,w,catOf[ci],st,rt,C);   /* rt/C filter local records to this coll */
    }
    int nrec = pdbw_count(w);
    int rc = kindCommit(w,kind,outpdb, ailen?ai:NULL, ailen);
    free(s);
    return rc<0 ? -1 : nrec;
}
