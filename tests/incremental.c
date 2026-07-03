/* incremental.c -- exercise the conflict-aware two-way sync against Radicale.
 *
 * Per policy (server/local/both) on a freshly-cleared calendar collection:
 *   1. seed 4 events, sync -> server has them, map written
 *   2. diverge:  local: modify u1, delete u2, add u6;  keep u3 clean
 *                server: modify u3, add u5;  and CONFLICT on u4 (both modify)
 *   3. sync with the policy, then assert LOCAL set == SERVER set (converged)
 *      and equals the policy's expected set
 *   4. sync again with no edits -> must be a total no-op (idempotent)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"

static DavCtx D;
static const char* COLL="palm/cal";
static const char* MAP ="state/inc.map";
static const char* LPDB="pdb/inc.pdb";
static int fails=0;

/* ---- build a datebook PDB from a compact spec ---- */
typedef struct { uint32_t uid; uint8_t attr; const char*sum; int y,m,d,h; } Ev;
static void writeDB(const char*path,const Ev*ev,int n){
    static uint8_t arena[64*PALM_REC_MAX]; static PdbRec r[64]; int used=0;
    for(int i=0;i<n;i++){
        Appt a; memset(&a,0,sizeof a);
        a.hasTime=1; a.sH=ev[i].h; a.sM=0; a.eH=ev[i].h+1; a.eM=0;
        a.year=ev[i].y; a.month=ev[i].m; a.day=ev[i].d;
        snprintf(a.description,sizeof a.description,"%s",ev[i].sum);
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=ev[i].attr,.uniqueID=ev[i].uid,.data=dst,.len=l }; used+=l;
    }
    pdb_write(path,"DatebookDB",0x44415441,0x64617465,r,n);
}

/* ---- put an event straight onto the server (simulates another client) ---- */
static void serverPut(uint32_t uid,const char*sum,int y,int m,int d,int h){
    Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=h;a.eH=h+1; a.year=y;a.month=m;a.day=d;
    snprintf(a.description,sizeof a.description,"%s",sum);
    char v[4096]; ical_emit(v,sizeof v,&a,uid);
    char obj[5000]; int n=snprintf(obj,sizeof obj,
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//test//EN\r\n%sEND:VCALENDAR\r\n",v);
    FILE*f=fopen("state/.tbody","wb"); fwrite(obj,1,n,f); fclose(f);
    char name[32]; snprintf(name,sizeof name,"%u.ics",uid);
    char etag[160]; int st=0;
    dav_put(&D,COLL,name,"text/calendar; charset=utf-8","state/.tbody",NULL,etag,sizeof etag,&st);
}

/* ---- collect summaries from local PDB and from server ---- */
typedef struct { char s[64][256]; int n; } Set;
static int addLocal(const PdbRec*r,int i,void*ctx){ (void)i; Set*S=ctx;
    Appt a; if(ApptUnpack(r->data,r->len,&a)) return 0;
    if(r->attr&REC_ATTR_DELETE) return 0;
    snprintf(S->s[S->n++],256,"%s",a.description);
    return 0; }
static void localSet(Set*S){ S->n=0; pdb_read(LPDB,addLocal,S); }

static void srvName(const char*name,const char*etag,void*ctx){ (void)etag; Set*S=ctx;
    if(strstr(name,".ics")) snprintf(S->s[S->n++],256,"%s",name); }
static void serverSet(Set*S){
    Set names={0}; dav_list(&D,COLL,srvName,&names); S->n=0;
    for(int i=0;i<names.n;i++){ char obj[16384];
        if(dav_get(&D,COLL,names.s[i],obj,sizeof obj)<=0) continue;
        Appt a; if(ical_parse(obj,&a)) continue; snprintf(S->s[S->n++],256,"%s",a.description); }
}
static int cmpstr(const void*a,const void*b){ return strcmp((const char*)a,(const char*)b); }
static void sortSet(Set*S){ qsort(S->s,S->n,sizeof S->s[0],cmpstr); }
static int eqSet(const Set*a,const Set*b){
    if(a->n!=b->n) return 0;
    for(int i=0;i<a->n;i++) if(strcmp(a->s[i],b->s[i])) return 0;
    return 1; }
static void printSet(const char*tag,const Set*s){
    printf("   %s(%d):",tag,s->n);
    for(int i=0;i<s->n;i++) printf(" %s",s->s[i]);
    printf("\n"); }

static void clearColl(void){
    Set names={0}; dav_list(&D,COLL,srvName,&names);
    for(int i=0;i<names.n;i++) dav_delete(&D,COLL,names.s[i],NULL);
    remove(MAP);
}

/* expected converged set per policy (u2 deleted throughout) */
static void expectSet(Set*e,ConflictPolicy pol){
    e->n=0;
    #define ADD(x) snprintf(e->s[e->n++],256,"%s",x)
    ADD("Alpha-loc");      /* u1 local mod pushed */
    ADD("Gamma-srv");      /* u3 server mod pulled */
    ADD("Epsilon");        /* u5 server new pulled */
    ADD("Zeta");           /* u6 local new pushed */
    if(pol==POL_SERVER)      ADD("Delta-srv");
    else if(pol==POL_LOCAL)  ADD("Delta-loc");
    else { ADD("Delta-srv"); ADD("Delta-loc"); }  /* both */
    #undef ADD
    sortSet(e);
}

static void CK(int cond,const char*msg){ if(!cond){ fails++; printf("   FAIL: %s\n",msg); } }

static void runPolicy(ConflictPolicy pol,const char*name){
    printf("== policy: %s ==\n",name);
    clearColl();
    /* 1. seed */
    Ev seed[]={ {1,0,"Alpha",2026,8,1,10},{2,0,"Beta",2026,8,2,10},
                {3,0,"Gamma",2026,8,3,10},{4,0,"Delta",2026,8,4,10} };
    writeDB(LPDB,seed,4);
    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,pol,&s0);
    CK(s0.pushNew==4,"seed pushes 4");

    /* 2. diverge -- local edits (rewrite PDB) */
    Ev after[]={ {1,0,"Alpha-loc",2026,8,1,10},        /* modified locally  */
                 {2,REC_ATTR_DELETE,"Beta",2026,8,2,10},/* deleted locally  */
                 {3,0,"Gamma",2026,8,3,10},             /* clean locally     */
                 {4,0,"Delta-loc",2026,8,4,10},         /* CONFLICT (local)  */
                 {6,0,"Zeta",2026,8,6,10} };            /* new locally       */
    writeDB(LPDB,after,5);
    /*    server edits */
    serverPut(3,"Gamma-srv",2026,8,3,10);   /* modified on server */
    serverPut(4,"Delta-srv",2026,8,4,10);   /* CONFLICT (server)  */
    serverPut(5,"Epsilon",2026,8,5,10);     /* new on server      */

    /* 3. sync with policy */
    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,pol,&s1);
    printf("   stats: push +%d ~%d -%d | pull +%d ~%d -%d | conflict %d | clean %d\n",
        s1.pushNew,s1.pushMod,s1.pushDel,s1.pullNew,s1.pullMod,s1.pullDel,s1.conflicts,s1.unchanged);
    CK(s1.conflicts==1,"exactly one conflict (u4)");
    CK(s1.pushMod==1,"one push-mod (u1)");
    CK(s1.pushDel==1,"one push-del (u2)");
    CK(s1.pullMod==1,"one pull-mod (u3)");
    CK(s1.pullNew==1,"one pull-new (u5)");
    CK(s1.pushNew==1,"one push-new (u6)");

    Set ls={0},ss={0},ex={0}; localSet(&ls); serverSet(&ss); expectSet(&ex,pol);
    sortSet(&ls); sortSet(&ss);
    printSet("local ",&ls); printSet("server",&ss); printSet("expect",&ex);
    CK(eqSet(&ls,&ss),"local and server CONVERGED");
    CK(eqSet(&ls,&ex),"converged to expected set");

    /* 4. idempotence: a second sync with no edits does nothing */
    SyncStats s2={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,pol,&s2);
    int ops=s2.pushNew+s2.pushMod+s2.pushDel+s2.pullNew+s2.pullMod+s2.pullDel+s2.conflicts;
    printf("   2nd sync ops=%d clean=%d\n",ops,s2.unchanged);
    CK(ops==0,"second sync is a no-op (idempotent)");
    CK(s2.unchanged==ex.n,"all records clean on second sync");
}

int main(void){
    snprintf(D.base,sizeof D.base,"%s",getenv("DAV_BASE")?getenv("DAV_BASE"):"http://localhost:5232");
    snprintf(D.user,sizeof D.user,"palm"); snprintf(D.pass,sizeof D.pass,"palm");
    runPolicy(POL_SERVER,"server-wins");
    runPolicy(POL_LOCAL ,"local-wins");
    runPolicy(POL_BOTH  ,"keep-both");
    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
