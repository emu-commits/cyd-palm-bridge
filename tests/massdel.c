/* massdel.c -- prove the MASS-DELETE GUARD actually fires, and that a sync
 * which fires it RESTORES the local database instead of erasing the server.
 *
 * Why this gate exists. The guard (sync.c, MASSDEL_MIN) is the last thing
 * standing between "the SD card had a bad read" and "the user's calendar is
 * gone from iCloud". Its POSITIVE path -- it triggers, and deletions are held
 * back -- had no test at all. It was covered only by the other gates staying
 * silent, which is not coverage: every one of them would have stayed exactly as
 * silent if the guard had been deleted outright.
 *
 * The shape the guard is built for is the one reproduced here: the map still
 * describes N records, and the local PDB no longer does. That is what an
 * unreadable card looks like from inside the engine -- pdb_read hands back
 * fewer records than it did last run, and every missing one presents as
 * "deleted locally since the last sync". With the guard off, this run would
 * issue N DELETEs against a perfectly healthy server.
 *
 * Three things are asserted, in the order they matter:
 *   1. the server still has all N objects (nothing was erased);
 *   2. no deletion was even counted (pushDel == 0);
 *   3. the NEXT sync repopulates the local database back to N -- the guard
 *      cannot merely decline to delete, it has to leave a state that heals.
 *
 * And then the negative control, without which the first three prove nothing:
 * a run BELOW the guard's threshold must still push a real deletion, or a guard
 * wired permanently on would pass every assertion above.
 *
 * Needs Radicale with palm/cal (tests/gate.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"

static DavCtx D;
static const char* COLL="palm/cal";
static const char* MAP ="state/mass.map";
static const char* LPDB="pdb/mass.pdb";
static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("   FAIL: %s\n",m); } }

/* MASSDEL_MIN is 8 in sync.c: the guard needs at least that many MAPPED records
 * before it will consider a shrink suspicious, and it fires when fewer than half
 * survive. 12 is comfortably over the line, and 12 -> 0 is the worst case. */
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
    for(int i=0;i<l.n;i++) dav_delete(&D,COLL,l.s[i],NULL);
    remove(MAP); remove(LPDB);
}

/* ---- the guard fires -------------------------------------------------- */
static void guardFires(void){
    printf("== the guard fires: %d mapped records, 0 local ==\n",NREC);
    clearColl();

    writeDB(NREC);
    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s0);
    CK(s0.pushNew==NREC,"seed pushed every record");
    CK(serverCount()==NREC,"server holds the seed");

    /* the card goes bad: the map still describes NREC, the PDB describes none */
    writeEmptyDB();
    CK(localCount()==0,"local database really is empty before the guarded run");

    SyncStats s1={0};
    int n = sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   rc=%d push -%d | pull +%d | server=%d local=%d\n",
           n,s1.pushDel,s1.pullNew,serverCount(),localCount());

    /* 1. THE POINT OF THE WHOLE GUARD. */
    CK(serverCount()==NREC,"SERVER UNTOUCHED -- no object was deleted");
    /* 2. not even attempted */
    CK(s1.pushDel==0,"no deletion was counted");
    CK(s1.bothDel==0,"and none was mislabelled as already-gone either");

    /* 3. IT HAS TO HEAL, NOT JUST DECLINE -- and in THIS run, not eventually.
     * This is the assertion that failed when the gate was first written: the
     * guard skipped the deletions and wrote nothing for those records, so the
     * device stayed empty and every later sync reached the same conclusion.
     * The server kept 12, the device kept 0, forever. */
    CK(n==NREC,"the guarded run writes a FULL database, not an empty one");
    CK(localCount()==NREC,"LOCAL DATABASE RESTORED from the server, in the guarded run");
    CK(s1.pullNew==NREC,"and every restored record is counted as a pull");

    /* ...and having healed, it settles: the next sync is an ordinary no-op with
     * the guard no longer firing. A restore that has to run every time is a
     * different bug wearing the same clothes. */
    SyncStats s2={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s2);
    int ops=s2.pushNew+s2.pushMod+s2.pushDel+s2.pullNew+s2.pullMod+s2.pullDel+s2.conflicts;
    printf("   next sync: ops=%d clean=%d server=%d local=%d\n",
           ops,s2.unchanged,serverCount(),localCount());
    CK(ops==0,"the sync after the restore is a no-op");
    CK(s2.unchanged==NREC,"all records clean once healed");
    CK(localCount()==NREC && serverCount()==NREC,"both sides whole and equal");
}

/* ---- the negative control --------------------------------------------- */
/* Without this, a guard stuck permanently ON would pass everything above. A
 * deletion small enough not to look like data loss must still go through. */
static void guardStaysOutOfTheWay(void){
    printf("== below the threshold: a real deletion still pushes ==\n");
    clearColl();

    writeDB(NREC);
    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s0);
    CK(serverCount()==NREC,"seed is on the server");

    /* Delete ONE record, the way the UI does: keep it in the PDB with the Palm
     * delete bit set. NREC-1 of NREC survive, which is nowhere near the guard's
     * "fewer than half" line. */
    static uint8_t arena[128*PALM_REC_MAX]; static PdbRec r[128]; int used=0;
    for(int i=0;i<NREC;i++){
        Appt a; memset(&a,0,sizeof a);
        a.hasTime=1; a.sH=9; a.eH=10; a.year=2026; a.month=9; a.day=1+(i%28);
        snprintf(a.description,sizeof a.description,"Event-%02d",i+1);
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=(uint8_t)(i==0?REC_ATTR_DELETE:0),
                       .uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l;
    }
    pdb_write(LPDB,"DatebookDB",0x44415441,0x64617465,r,NREC);

    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   push -%d | server=%d\n",s1.pushDel,serverCount());
    CK(s1.pushDel==1,"the one genuine deletion was pushed");
    CK(serverCount()==NREC-1,"and the server is one object lighter");
}

/* ---- deleted on both sides is not a push ------------------------------ */
/* The third of the three correctness items: LDEL+SDEL issues no network call,
 * so it must not be counted as a pushDel. Delete one record locally AND remove
 * its object from the server by hand, then sync. */
static void deletedOnBothSides(void){
    printf("== deleted on both sides counts as gone, not as a push ==\n");
    clearColl();

    writeDB(NREC);
    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s0);
    CK(serverCount()==NREC,"seed is on the server");

    /* take uid 1 off the server behind the engine's back */
    Set l={0}; dav_list(&D,COLL,srvName,&l);
    int removed=0;
    for(int i=0;i<l.n;i++) if(strstr(l.s[i],"1.ics") && !strstr(l.s[i],"11.ics")
                              && !strstr(l.s[i],"21.ics")){
        dav_delete(&D,COLL,l.s[i],NULL); removed=1; break; }
    CK(removed,"removed one object from the server directly");

    /* ...and delete the same record locally */
    static uint8_t arena[128*PALM_REC_MAX]; static PdbRec r[128]; int used=0;
    for(int i=0;i<NREC;i++){
        Appt a; memset(&a,0,sizeof a);
        a.hasTime=1; a.sH=9; a.eH=10; a.year=2026; a.month=9; a.day=1+(i%28);
        snprintf(a.description,sizeof a.description,"Event-%02d",i+1);
        uint8_t*dst=arena+used; int l2=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=(uint8_t)(i==0?REC_ATTR_DELETE:0),
                       .uniqueID=(uint32_t)(i+1),.data=dst,.len=l2 }; used+=l2;
    }
    pdb_write(LPDB,"DatebookDB",0x44415441,0x64617465,r,NREC);

    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   push -%d | already gone %d | server=%d\n",
           s1.pushDel,s1.bothDel,serverCount());
    CK(s1.pushDel==0,"nothing was PUSHED -- there was nothing on the server to delete");
    CK(s1.bothDel==1,"it is counted as already-gone instead");
}

/* ---- a tombstone is not a bad read ------------------------------------- */
/* The device keeps a deleted record as a Palm tombstone until it has synced.
 * That is what lets the engine tell the two shapes apart: a record the USER
 * deleted is still in the database, flagged; a record the CARD lost is simply
 * absent. So even while the guard is up -- here 5 tombstones and 7 records
 * missing out of 12 mapped, which is fewer than half -- the five explicit
 * deletions must be pushed and only the seven silent losses restored. */
static void tombstonesUnderTheGuard(void){
    printf("== the guard fires, but tombstones are still deletions ==\n");
    clearColl();
    writeDB(NREC);
    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s0);
    CK(serverCount()==NREC,"seed is on the server");
    static uint8_t arena[128*PALM_REC_MAX]; static PdbRec r[128]; int used=0;
    const int NT = 5;
    for(int i=0;i<NT;i++){
        Appt a; memset(&a,0,sizeof a);
        a.hasTime=1; a.sH=9; a.eH=10; a.year=2026; a.month=9; a.day=1+(i%28);
        snprintf(a.description,sizeof a.description,"Event-%02d",i+1);
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=REC_ATTR_DELETE,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l;
    }
    pdb_write(LPDB,"DatebookDB",0x44415441,0x64617465,r,NT);
    SyncStats s1={0};
    int n = sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("   rc=%d push -%d | pull +%d | server=%d local=%d\n",
           n,s1.pushDel,s1.pullNew,serverCount(),localCount());
    CK(s1.pushDel==NT,"every tombstone was pushed as a deletion, guard or no guard");
    CK(serverCount()==NREC-NT,"so the server lost exactly those");
    CK(localCount()==NREC-NT,"and the silently missing ones were restored");
    CK(s1.pullNew==NREC-NT,"counted as pulls");
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
    guardFires();
    guardStaysOutOfTheWay();
    deletedOnBothSides();
    tombstonesUnderTheGuard();
    heldStayLocal();
    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
