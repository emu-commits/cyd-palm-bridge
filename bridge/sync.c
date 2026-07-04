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
#include "sync.h"

/* ======================= full-sync primitives ========================== */
typedef struct { const DavCtx*d; const char*coll; int isCal; FILE*map; int n; } PushCtx;

static int emit_object(int isCal,const uint8_t*data,int len,uint32_t uid,char*out,int cap){
    if(isCal){
        Appt a; if(ApptUnpack(data,len,&a)) return -1;
        char v[4096]; ical_emit(v,sizeof v,&a,uid);
        char vt[1200]; int vl=(a.hasTime)?ical_vtimezone(vt,sizeof vt):0; if(vl<0)vl=0; if(!vl)vt[0]=0;
        return snprintf(out,cap,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//CYD-Palm-Bridge//EN\r\n%s%sEND:VCALENDAR\r\n",vt,v);
    } else {
        Addr a; if(AddrUnpack(data,len,&a)) return -1;
        return vcard_emit(out,cap,&a,uid);
    }
}

static int pushRec(const PdbRec*r,int i,void*ctx){
    (void)i; PushCtx*p=ctx;
    char body[8192]; int bl=emit_object(p->isCal,r->data,r->len,r->uniqueID,body,sizeof body);
    if(bl<0) return 0;
    FILE*f=fopen("state/.body","wb"); fwrite(body,1,strlen(body),f); fclose(f);
    char name[64]; snprintf(name,sizeof name,"%u.%s",r->uniqueID,p->isCal?"ics":"vcf");
    char etag[160]=""; int st=0;
    dav_put(p->d,p->coll,name,p->isCal?"text/calendar; charset=utf-8":"text/vcard; charset=utf-8",
            "state/.body",NULL,etag,sizeof etag,&st);
    fprintf(p->map,"%s\t%u\t%s\t%s\n",p->isCal?"cal":"card",r->uniqueID,name,etag);
    p->n++;
    return 0;
}

int sync_push(const DavCtx*d,const char*pdbpath,const char*coll,int isCal){
    FILE*map=fopen("state/sync_map.tsv","a");
    PushCtx p={ .d=d,.coll=coll,.isCal=isCal,.map=map,.n=0 };
    pdb_read(pdbpath,pushRec,&p);
    if(map) fclose(map);
    return p.n;
}

typedef struct { char names[256][256]; int n; } NameList;
static void collectName(const char*name,const char*etag,void*ctx){
    (void)etag; NameList*l=ctx; if(l->n<256) snprintf(l->names[l->n++],256,"%s",name);
}

int sync_pull(const DavCtx*d,const char*coll,const char*outpdb,int isCal){
    NameList nl={0}; dav_list(d,coll,collectName,&nl);
    static uint8_t arena[256*PALM_REC_MAX]; static PdbRec recs[256];
    int nrec=0, used=0;
    for(int i=0;i<nl.n;i++){
        const char*nm=nl.names[i]; const char*dot=strrchr(nm,'.'); if(!dot) continue;
        if(isCal!=!strcmp(dot,".ics")) {} /* keep only matching */
        if(isCal && strcmp(dot,".ics")) continue;
        if(!isCal && strcmp(dot,".vcf")) continue;
        char obj[16384]; if(dav_get(d,coll,nm,obj,sizeof obj)<=0) continue;
        uint32_t uid=(uint32_t)strtoul(nm,NULL,10);
        uint8_t*dst=arena+used; int l;
        if(isCal){ Appt a; if(ical_parse(obj,&a)) continue; l=ApptPack(dst,PALM_REC_MAX,&a); }
        else     { Addr a; if(vcard_parse(obj,&a)) continue; l=AddrPack(dst,PALM_REC_MAX,&a); }
        if(l<=0) continue;
        recs[nrec]=(PdbRec){ .attr=0,.uniqueID=uid,.data=dst,.len=l }; used+=l; nrec++;
        if(nrec>=256||used+PALM_REC_MAX>(int)sizeof arena) break;
    }
    for(int i=1;i<nrec;i++){ PdbRec k=recs[i]; int j=i-1;
        while(j>=0 && recs[j].uniqueID>k.uniqueID){ recs[j+1]=recs[j]; j--; } recs[j+1]=k; }
    if(isCal) pdb_write(outpdb,"DatebookDB",0x44415441,0x64617465,recs,nrec);
    else      pdb_write(outpdb,"AddressDB",0x44415441,0x61646472,recs,nrec);
    return nrec;
}

/* ==================== incremental conflict-aware sync =================== */
#define MAXR 256

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
    int isCal;
    Loc loc[MAXR]; int nloc; uint8_t locArena[MAXR*PALM_REC_MAX]; int locUsed;
    Map map[MAXR]; int nmap;
    Srv srv[MAXR]; int nsrv;
    char token[1200];                  /* RFC 6578 sync-token from last run   */
    char newToken[1200];               /* token to persist after this run     */
} S;

static int loadRec(const PdbRec*r,int i,void*ctx){
    (void)i; S*s=ctx; if(s->nloc>=MAXR) return 1;
    Loc*L=&s->loc[s->nloc];
    L->uid=r->uniqueID; L->attr=r->attr; L->len=r->len; L->off=s->locUsed;
    memcpy(s->locArena+s->locUsed,r->data,r->len); s->locUsed+=r->len;
    char body[8192]; if(emit_object(s->isCal,r->data,r->len,r->uniqueID,body,sizeof body)<0) L->hash=0;
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
        Map m; memset(&m,0,sizeof m); unsigned long long h=0;
        if(sscanf(line,"%u\t%63[^\t]\t%159[^\t]\t%llu",&m.uid,m.href,m.etag,&h)>=3){
            m.hash=(uint64_t)h; if(s->nmap<MAXR) s->map[s->nmap++]=m;
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

/* ---- output accumulator: kept records -> merged PDB + refreshed map ---- */
typedef struct {
    PdbRec rec[MAXR]; int nrec; uint8_t arena[MAXR*PALM_REC_MAX]; int used;
    Map map[MAXR]; int nmap;
} Out;

static void keepBytes(Out*o,uint32_t uid,uint8_t attr,const uint8_t*data,int len,
                      const char*href,const char*etag,uint64_t hash){
    uint8_t*dst=o->arena+o->used; memcpy(dst,data,len);
    o->rec[o->nrec]=(PdbRec){ .attr=(uint8_t)(attr&~REC_ATTR_DIRTY&~REC_ATTR_DELETE),
                              .uniqueID=uid,.data=dst,.len=len };
    o->used+=len; o->nrec++;
    Map*m=&o->map[o->nmap++]; m->uid=uid; snprintf(m->href,sizeof m->href,"%s",href);
    snprintf(m->etag,sizeof m->etag,"%s",etag); m->hash=hash;
}

/* GET a server object, parse+pack into a local record, keep it. */
static int keepFromServer(const DavCtx*d,const char*coll,int isCal,Out*o,
                          uint32_t uid,const char*name,const char*etag){
    char obj[16384]; if(dav_get(d,coll,name,obj,sizeof obj)<=0) return -1;
    uint8_t tmp[PALM_REC_MAX]; int l;
    if(isCal){ Appt a; if(ical_parse(obj,&a)) return -1; l=ApptPack(tmp,sizeof tmp,&a); }
    else     { Addr a; if(vcard_parse(obj,&a)) return -1; l=AddrPack(tmp,sizeof tmp,&a); }
    if(l<=0) return -1;
    char body[8192]; emit_object(isCal,tmp,l,uid,body,sizeof body);
    keepBytes(o,uid,0,tmp,l,name,etag,fnv1a(body));
    return 0;
}

/* PUT a local record; ifmatch NULL = unconditional. keep it locally. */
static int pushLocal(const DavCtx*d,const char*coll,int isCal,Out*o,
                     const Loc*L,const uint8_t*arena,uint32_t uid,
                     const char*name,const char*ifmatch){
    char body[8192]; int bl=emit_object(isCal,arena+L->off,L->len,uid,body,sizeof body);
    if(bl<0) return -1;
    FILE*f=fopen("state/.body","wb"); fwrite(body,1,strlen(body),f); fclose(f);
    char etag[160]=""; int st=0;
    dav_put(d,coll,name,isCal?"text/calendar; charset=utf-8":"text/vcard; charset=utf-8",
            "state/.body",ifmatch,etag,sizeof etag,&st);
    if(!etag[0]) dav_getetag(d,coll,name,etag,sizeof etag);   /* fallback */
    keepBytes(o,uid,L->attr,arena+L->off,L->len,name,etag,fnv1a(body));
    return st;
}

int sync_collection(const DavCtx*d,const char*localpdb,const char*outpdb,
                    const char*coll,int isCal,const char*mapfile,
                    ConflictPolicy pol,SyncStats*st){
    static S s; memset(&s,0,sizeof s); s.isCal=isCal;
    pdb_read(localpdb,loadRec,&s);
    loadMap(&s,mapfile);
    buildServer(&s,d,coll);

    static Node nodes[MAXR*2]; int nn=0;
    uint32_t seed=1;
    for(int i=0;i<s.nloc;i++){ Node*n=nodeFor(nodes,&nn,s.loc[i].uid); n->li=i; if(s.loc[i].uid>=seed)seed=s.loc[i].uid+1; }
    for(int i=0;i<s.nmap;i++){ Node*n=nodeFor(nodes,&nn,s.map[i].uid); n->hasMap=1;
        snprintf(n->mHref,sizeof n->mHref,"%s",s.map[i].href);
        snprintf(n->mEtag,sizeof n->mEtag,"%s",s.map[i].etag); n->mHash=s.map[i].hash;
        if(s.map[i].uid>=seed)seed=s.map[i].uid+1; }
    for(int i=0;i<s.nsrv;i++){ if(!s.srv[i].present) continue;   /* deleted on server */
        Node*n=nodeFor(nodes,&nn,s.srv[i].uid); n->hasSrv=1;
        snprintf(n->sName,sizeof n->sName,"%s",s.srv[i].name);
        snprintf(n->sEtag,sizeof n->sEtag,"%s",s.srv[i].etag);
        if(s.srv[i].uid>=seed)seed=s.srv[i].uid+1; }

    static Out o; memset(&o,0,sizeof o);
    SyncStats z={0}; if(!st) st=&z;
    const char*ext = isCal?"ics":"vcf";

    for(int i=0;i<nn;i++){
        Node*n=&nodes[i];
        const Loc*L = n->li>=0 ? &s.loc[n->li] : NULL;
        int ldel = L && (L->attr & REC_ATTR_DELETE);

        /* classify local */
        enum { LABSENT, LNEW, LMOD, LDEL, LCLEAN } lc;
        if(!L)               lc=LABSENT;
        else if(ldel)        lc=LDEL;
        else if(!n->hasMap)  lc=LNEW;
        else if(L->hash!=n->mHash) lc=LMOD;
        else                 lc=LCLEAN;

        /* classify server */
        enum { SABSENT, SNEW, SMOD, SDEL, SCLEAN } sc;
        if(!n->hasMap &&  n->hasSrv) sc=SNEW;
        else if(!n->hasMap)          sc=SABSENT;
        else if(!n->hasSrv)          sc=SDEL;
        else if(strcmp(n->sEtag,n->mEtag)) sc=SMOD;
        else                         sc=SCLEAN;

        char lname[64]; snprintf(lname,sizeof lname,"%u.%s",n->uid,ext);
        const char*srvName = n->hasSrv?n->sName:(n->hasMap?n->mHref:lname);

        int conflict = (lc==LMOD||lc==LDEL||lc==LNEW) && (sc==SMOD||sc==SNEW||sc==SDEL)
                       && !(lc==LNEW&&sc==SABSENT) && !(lc==LDEL&&sc==SDEL);

        if(conflict){
            st->conflicts++;
            int serverWins = (pol==POL_SERVER);
            int localWins  = (pol==POL_LOCAL);
            /* modify-beats-delete for POL_BOTH and as the humane default edge */
            if(pol==POL_BOTH){
                if(sc==SDEL){ localWins=1; }          /* local mod survives   */
                else if(lc==LDEL){ serverWins=1; }    /* server mod survives   */
                else {                                 /* true mod/mod or new/new: keep both */
                    if(sc!=SDEL) keepFromServer(d,coll,isCal,&o,n->uid,srvName,n->sEtag);
                    if(L && lc!=LDEL){ uint32_t u2=seed++; char n2[64]; snprintf(n2,sizeof n2,"%u.%s",u2,ext);
                        pushLocal(d,coll,isCal,&o,L,s.locArena,u2,n2,NULL); }
                    continue;
                }
            }
            if(serverWins){
                if(sc==SDEL){ /* server deleted -> honor delete */ if(n->hasMap) {} }
                else keepFromServer(d,coll,isCal,&o,n->uid,srvName,n->sEtag);
            } else if(localWins){
                if(lc==LDEL){ dav_delete(d,coll,srvName,NULL); }
                else pushLocal(d,coll,isCal,&o,L,s.locArena,n->uid,lname,NULL);
            }
            continue;
        }

        /* ---- no-conflict matrix ---- */
        if(lc==LNEW && sc==SABSENT){
            pushLocal(d,coll,isCal,&o,L,s.locArena,n->uid,lname,NULL); st->pushNew++;
        } else if(lc==LMOD && sc==SCLEAN){
            pushLocal(d,coll,isCal,&o,L,s.locArena,n->uid,srvName,n->mEtag); st->pushMod++;
        } else if(lc==LCLEAN && sc==SCLEAN){
            keepBytes(&o,n->uid,L->attr,s.locArena+L->off,L->len,srvName,n->mEtag,L->hash); st->unchanged++;
        } else if(lc==LCLEAN && sc==SMOD){
            keepFromServer(d,coll,isCal,&o,n->uid,srvName,n->sEtag); st->pullMod++;
        } else if(lc==LCLEAN && sc==SDEL){
            st->pullDel++; /* drop local, remove map */
        } else if(lc==LDEL && (sc==SCLEAN)){
            dav_delete(d,coll,srvName,n->mEtag); st->pushDel++;
        } else if(lc==LDEL && sc==SDEL){
            st->pushDel++;
        } else if(lc==LABSENT && sc==SNEW){
            keepFromServer(d,coll,isCal,&o,n->uid,srvName,n->sEtag); st->pullNew++;
        } else if(lc==LABSENT && sc==SCLEAN){
            /* record removed locally without tombstone -> treat as local delete */
            dav_delete(d,coll,srvName,n->mEtag); st->pushDel++;
        } else if(lc==LABSENT && sc==SDEL){
            /* gone on both, just forget */
        }
        /* remaining combos are contradictions; skip */
    }

    /* sort merged records by uid, write PDB */
    for(int i=1;i<o.nrec;i++){ PdbRec k=o.rec[i]; Map km=o.map[i]; int j=i-1;
        while(j>=0 && o.rec[j].uniqueID>k.uniqueID){ o.rec[j+1]=o.rec[j]; o.map[j+1]=o.map[j]; j--; }
        o.rec[j+1]=k; o.map[j+1]=km; }
    if(isCal) pdb_write(outpdb,"DatebookDB",0x44415441,0x64617465,o.rec,o.nrec);
    else      pdb_write(outpdb,"AddressDB",0x44415441,0x61646472,o.rec,o.nrec);

    /* rewrite the map (sync-token header first, then one row per kept record) */
    FILE*mf=fopen(mapfile,"w");
    if(mf){
        if(s.newToken[0]) fprintf(mf,"#synctoken\t%s\n",s.newToken);
        for(int i=0;i<o.nmap;i++) fprintf(mf,"%u\t%s\t%s\t%llu\n",
                o.map[i].uid,o.map[i].href,o.map[i].etag,(unsigned long long)o.map[i].hash);
        fclose(mf); }
    return o.nrec;
}
