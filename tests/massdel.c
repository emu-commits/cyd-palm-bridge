/* massdel.c -- nothing on the device can delete anything on the server.
 *
 * The bridge has no DELETE (bridge/dav.h). What used to be pushed as a
 * deletion is now handled on the device alone, and this gate pins the two
 * shapes that matter for a real account:
 *
 *   A record MISSING from the local database (no tombstone) is the card having
 *   lost it -- a crash mid-write, a short read. It is RESTORED from the server,
 *   in the same run, whether one record went or all of them. (This used to be
 *   the mass-delete guard, which only held back pushing such deletions when
 *   most of a collection vanished at once.)
 *
 *   A record the USER deleted on the device (a tombstone, REC_ATTR_DELETE)
 *   leaves the device and is never deleted on the server. Its map row is kept
 *   as hidden, so the next sync does not pull it back -- unless the server's
 *   copy changes, when it comes back.
 *
 * Every scenario checks the server's object count, because that is the
 * number a bug here would change on someone's iCloud.
 *
 * Needs Radicale with palm/cal (tests/gate.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"
#include "radicale_rm.h"

static DavCtx D;
static const char* COLL="palm/cal";
static const char* MAP ="state/mass.map";
static const char* LPDB="pdb/mass.pdb";
static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("   FAIL: %s\n",m); } }

/* Over the old mass-delete guard's line (8 mapped, fewer than half left), so
 * the worst case, 12 -> 0, is the one it used to handle. */
#define NREC 12

typedef struct { char s[128][256]; int n; } Set;

static void srvName(const char*name,const char*etag,void*ctx){ (void)etag; Set*S=ctx;
    if(strstr(name,".ics") && S->n<128) snprintf(S->s[S->n++],256,"%s",name); }
static int serverCount(void){ Set l={0}; dav_list(&D,COLL,srvName,&l); return l.n; }

static int countCb(const PdbRec*r,int i,void*c){ (void)i;
    if(!(r->attr & REC_ATTR_DELETE)) (*(int*)c)++;
    return 0;
}
static int localCount(void){ int n=0; pdb_read(LPDB,countCb,&n); return n; }

/* write a datebook PDB holding `n` live events, uids 1..n */
static void writeDB(int n){
    static uint8_t arena[128*PALM_REC_MAX]; static PdbRec r[128]; int used=0;
    for(int i=0;i<n;i++){
        Appt a; memset(&a,0,sizeof a);
        a.hasTime=1; a.sH=9; a.eH=10; a.year=2026; a.month=9; a.day=1+(i%28);
        snprintf(a.description,sizeof a.description,"Event-%02d",i+1);
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l;
    }
    pdb_write(LPDB,"DatebookDB",0x44415441,0x64617465,r,n);
}

/* An EMPTY but structurally valid PDB -- the map still names NREC records and
 * the database names none. Writing zero records is how a card that read badly
 * presents itself; the engine cannot tell this from a user who deleted them. */
static void writeEmptyDB(void){
    pdb_write(LPDB,"DatebookDB",0x44415441,0x64617465,NULL,0);
}

static void clearColl(void){
    Set l={0}; dav_list(&D,COLL,srvName,&l);
    for(int i=0;i<l.n;i++) test_rm(&D,COLL,l.s[i],NULL);
    remove(MAP); remove(LPDB);
}

/* write NREC events, the first `nt` as tombstones; with `keep` < NREC, only
 * the first `keep` are written at all (the rest are "lost by the card"). */
static void writeMixed(int nt,int keep){
    static uint8_t arena[128*PALM_REC_MAX]; static PdbRec r[128]; int used=0;
    for(int i=0;i<keep;i++){
        Appt a; memset(&a,0,sizeof a);
        a.hasTime=1; a.sH=9; a.eH=10; a.year=2026; a.month=9; a.day=1+(i%28);
        snprintf(a.description,sizeof a.description,"Event-%02d",i+1);
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=(uint8_t)(i<nt?REC_ATTR_DELETE:0),
                       .uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l;
    }
    pdb_write(LPDB,"DatebookDB",0x44415441,0x64617465,r,keep);
}
static void seed(void){
    clearColl();
    writeDB(NREC);
    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s0);
    CK(s0.pushNew==NREC,"seed pushed every record");
    CK(serverCount()==NREC,"server holds the seed");
}
static int opsOf(const SyncStats*s){
    return s->pushNew+s->pushMod+s->pullNew+s->pullMod+s->pullDel+s->conflicts; }

/* the server object holding uid 1 ("1.ics"; 11.ics and 21.ics also end so) */
static int findOne(char*out,int cap){
    Set l={0}; dav_list(&D,COLL,srvName,&l);
    for(int i=0;i<l.n;i++){ const char*b=strrchr(l.s[i],'/'); b=b?b+1:l.s[i];
        if(!strcmp(b,"1.ics")){ snprintf(out,cap,"%s",l.s[i]); return 1; } }
    return 0;
}

/* ---- the whole database lost: restored, server untouched -------------- */
static void lostDatabase(void){
    printf("== the card loses the database: %d mapped records, 0 local ==\n",NREC);
    seed();
    writeEmptyDB();
    CK(localCount()==0,"local database really is empty before the run");

    SyncStats s1={0};
    int n = sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   rc=%d pull +%d | server=%d local=%d\n",n,s1.pullNew,serverCount(),localCount());
    CK(serverCount()==NREC,"SERVER UNTOUCHED -- no object was deleted");
    CK(s1.hidden==0 && s1.bothDel==0,"nothing counted as a deletion of any kind");
    CK(n==NREC && localCount()==NREC,"LOCAL DATABASE RESTORED from the server, in this run");
    CK(s1.pullNew==NREC,"and every restored record is counted as a pull");

    SyncStats s2={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s2);
    CK(opsOf(&s2)==0 && s2.unchanged==NREC,"the sync after the restore is a no-op");
}

/* ---- one record lost: restored too (no threshold any more) ------------- */
static void oneRecordLost(void){
    printf("== the card loses one record of %d ==\n",NREC);
    seed();
    writeMixed(0,NREC-1);                    /* uid NREC simply missing */
    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   pull +%d | server=%d local=%d\n",s1.pullNew,serverCount(),localCount());
    CK(serverCount()==NREC,"SERVER UNTOUCHED");
    CK(s1.pullNew==1 && localCount()==NREC,"the missing record is restored");
}

/* ---- a delete on the device stays on the device ------------------------ */
static void deviceDeleteStaysLocal(void){
    printf("== a delete on the device never reaches the server ==\n");
    seed();
    writeMixed(1,NREC);                      /* uid 1 deleted the way the UI does */
    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   hidden %d | server=%d local=%d\n",s1.hidden,serverCount(),localCount());
    CK(s1.hidden==1,"the one deletion is counted as hidden");
    CK(serverCount()==NREC,"SERVER UNTOUCHED -- it still has the deleted record");
    CK(localCount()==NREC-1,"the device no longer has it");

    SyncStats s2={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s2);
    CK(opsOf(&s2)==0 && s2.hidden==0,"the next sync is a no-op");
    CK(localCount()==NREC-1,"and does not pull the hidden record back");

    /* edited in the cloud afterwards: it is live again, so it comes back */
    char nm[256]=""; CK(findOne(nm,sizeof nm),"found uid 1 on the server");
    static char obj[16384]; int got=dav_get(&D,COLL,nm,obj,sizeof obj);
    char*at = got>0 ? strstr(obj,"Event-01") : NULL;
    CK(at!=NULL,"fetched it");
    if(at){
        char ed[16400]; snprintf(ed,sizeof ed,"%.*sEvent-01 edited%s",(int)(at-obj),obj,at+8);
        FILE*f=fopen("state/.mdel","wb"); if(f){ fputs(ed,f); fclose(f); }
        char etag[160]; int st=0;
        dav_put(&D,COLL,nm,"text/calendar; charset=utf-8","state/.mdel",NULL,etag,sizeof etag,&st);
        CK(st>=200 && st<300,"edited on the server");
    }
    SyncStats s3={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s3);
    printf("   after a server edit: pull +%d | local=%d\n",s3.pullNew,localCount());
    CK(s3.pullNew==1 && localCount()==NREC,"a server edit brings the hidden record back");
    CK(serverCount()==NREC,"and the server still has all of them");
}

/* ---- deleted on both sides --------------------------------------------- */
static void deletedOnBothSides(void){
    printf("== deleted on both sides counts as gone ==\n");
    seed();
    char nm[256]=""; CK(findOne(nm,sizeof nm),"found uid 1 on the server");
    test_rm(&D,COLL,nm,NULL);                /* behind the engine's back */
    writeMixed(1,NREC);
    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   hidden %d | already gone %d | server=%d\n",s1.hidden,s1.bothDel,serverCount());
    CK(s1.hidden==0 && s1.bothDel==1,"counted as already gone, not hidden");
    CK(serverCount()==NREC-1,"the server lost only what was removed by hand");
}

/* ---- tombstones and a lost database at once ----------------------------- */
/* 5 tombstones and 7 records missing out of 12 mapped: the five the user
 * deleted leave the device, the seven the card lost come back, and the server
 * keeps all twelve. */
static void tombstonesAndLosses(void){
    printf("== tombstones and losses together ==\n");
    seed();
    const int NT = 5;
    writeMixed(NT,NT);
    SyncStats s1={0};
    int n = sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   rc=%d hidden %d | pull +%d | server=%d local=%d\n",
           n,s1.hidden,s1.pullNew,serverCount(),localCount());
    CK(s1.hidden==NT,"every tombstone hidden on the device");
    CK(serverCount()==NREC,"SERVER UNTOUCHED");
    CK(localCount()==NREC-NT && s1.pullNew==NREC-NT,"the silently missing ones were restored");
}

/* ---- held records (the demo seed) stay on the device ------------------ */
/* The device holds its demo seed back from every push. A held record must be
 * carried through the merge untouched -- the output PDB only contains what the
 * merge writes, so "not pushed" must not turn into "dropped" -- and it must
 * still be held on the next run, not pushed then. */
static int holdLow(uint32_t uid, void *ctx){ (void)ctx; return uid <= 2; }
static void heldStayLocal(void){
    printf("== held records stay local, unpushed, run after run ==\n");
    clearColl();
    writeDB(4);                                  /* uids 1..4; 1 and 2 are "demo" */
    sync_set_hold(holdLow, NULL);
    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   push +%d held %d | server=%d local=%d\n",s1.pushNew,s1.held,serverCount(),localCount());
    CK(s1.pushNew==2 && s1.held==2,"two pushed, two held");
    CK(serverCount()==2,"the held ones never reached the server");
    CK(localCount()==4,"and all four are still on the device");
    SyncStats s2={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s2);
    CK(s2.pushNew==0 && s2.held==2 && serverCount()==2 && localCount()==4,
       "the next run holds them again and changes nothing");
    sync_set_hold(NULL, NULL);
}

int main(void){
    snprintf(D.base,sizeof D.base,"%s",
             getenv("DAV_BASE")?getenv("DAV_BASE"):"http://localhost:5232");
    snprintf(D.user,sizeof D.user,"palm"); snprintf(D.pass,sizeof D.pass,"palm");
    lostDatabase();
    oneRecordLost();
    deviceDeleteStaysLocal();
    deletedOnBothSides();
    tombstonesAndLosses();
    heldStayLocal();
    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
