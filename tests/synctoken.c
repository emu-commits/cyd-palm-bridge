/* synctoken.c -- prove RFC 6578 sync-collection behaviour against Radicale.
 *
 *  A. dav_sync_report deltas: initial full -> token; empty delta; then an
 *     exact {1 changed, 1 deleted} delta after a server-side edit+delete.
 *  B. invalid/expired token -> caller told to full-resync.
 *  C. sync_collection persists the token (map header) and a no-change second
 *     sync is a delta no-op.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"

static DavCtx D;
static const char* COLL="palm/synctest";
static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("  FAIL: %s\n",m);} }

/* recreate the test collection so sync-token history starts pristine */
static void freshColl(void){
    char cmd[512];
    snprintf(cmd,sizeof cmd,"curl -s -o /dev/null -u %s:%s -X DELETE '%s/%s/'",D.user,D.pass,D.base,COLL);
    if(system(cmd)){}
    snprintf(cmd,sizeof cmd,"curl -s -o /dev/null -u %s:%s -X MKCALENDAR '%s/%s/'",D.user,D.pass,D.base,COLL);
    if(system(cmd)){}
}

/* collectors for dav_sync_report */
typedef struct { int changed, deleted; char last[64]; } Rep;
static void repCb(const char*name,const char*etag,int del,void*ctx){
    (void)etag; Rep*r=ctx; if(del) r->deleted++; else r->changed++;
    snprintf(r->last,sizeof r->last,"%s",name);
}

static void putEvent(const char*name,const char*summary){
    char obj[512]; snprintf(obj,sizeof obj,
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//t//EN\r\nBEGIN:VEVENT\r\n"
        "UID:%s\r\nDTSTART:20260901T090000\r\nDTEND:20260901T100000\r\nSUMMARY:%s\r\n"
        "END:VEVENT\r\nEND:VCALENDAR\r\n",name,summary);
    FILE*f=fopen("state/.tput","wb"); fwrite(obj,1,strlen(obj),f); fclose(f);
    char etag[160]; int st=0;
    dav_put(&D,COLL,name,"text/calendar; charset=utf-8","state/.tput",NULL,etag,sizeof etag,&st);
}
static void testReportDeltas(void){
    printf("== A. dav_sync_report deltas ==\n");
    freshColl();
    putEvent("a.ics","A"); putEvent("b.ics","B"); putEvent("c.ics","C");

    /* initial (empty token) -> full listing + a token */
    Rep r1={0}; char t1[1200]="";
    int rc1=dav_sync_report(&D,COLL,"",repCb,&r1,t1,sizeof t1);
    printf("  initial: rc=%d changed=%d deleted=%d token=%.24s...\n",rc1,r1.changed,r1.deleted,t1);
    CK(rc1==0,"initial report ok");
    CK(t1[0]!=0,"server returned a sync-token");
    CK(r1.changed==3,"initial reports 3 members");

    /* no changes -> delta must be empty */
    Rep r2={0}; char t2[1200]="";
    int rc2=dav_sync_report(&D,COLL,t1,repCb,&r2,t2,sizeof t2);
    printf("  no-change delta: rc=%d changed=%d deleted=%d\n",rc2,r2.changed,r2.deleted);
    CK(rc2==0,"delta report ok");
    CK(r2.changed==0&&r2.deleted==0,"empty delta when nothing changed");

    /* one change + one delete -> delta reports exactly those two */
    putEvent("b.ics","B-modified");      /* change */
    dav_delete(&D,COLL,"c.ics",NULL);    /* delete */
    const char*tok = t2[0]?t2:t1;
    Rep r3={0}; char t3[1200]="";
    int rc3=dav_sync_report(&D,COLL,tok,repCb,&r3,t3,sizeof t3);
    printf("  after edit+delete: rc=%d changed=%d deleted=%d\n",rc3,r3.changed,r3.deleted);
    CK(rc3==0,"delta report ok");
    CK(r3.changed==1,"exactly one changed member in delta");
    CK(r3.deleted==1,"exactly one deleted member in delta");
}

static void testInvalidToken(void){
    printf("== B. invalid/expired token ==\n");
    Rep r={0}; char t[1200]="";
    int rc=dav_sync_report(&D,COLL,"http://radicale.org/ns/sync/deadbeefbogus",repCb,&r,t,sizeof t);
    printf("  bogus token: rc=%d (1=invalid->resync, -1=unsupported)\n",rc);
    CK(rc!=0,"bogus token does NOT return success (forces full resync)");
    /* recovery: a fresh full report still works */
    Rep r2={0}; char t2[1200]="";
    int rc2=dav_sync_report(&D,COLL,"",repCb,&r2,t2,sizeof t2);
    CK(rc2==0&&t2[0]!=0,"full resync recovers with a new token");
}

/* build a datebook PDB with n events uid 1..n */
static void writeDB(const char*path,int n){
    static uint8_t arena[16*PALM_REC_MAX]; static PdbRec r[16]; int used=0;
    for(int i=0;i<n;i++){ Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=9;a.eH=10;
        a.year=2026;a.month=9;a.day=1+i; snprintf(a.description,sizeof a.description,"E%d",i+1);
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l; }
    pdb_write(path,"DatebookDB",0x44415441,0x64617465,r,n);
}
static int mapHasToken(const char*mapfile){
    FILE*f=fopen(mapfile,"r"); if(!f) return 0; char line[1400]; int found=0;
    while(fgets(line,sizeof line,f)) if(!strncmp(line,"#synctoken\t",11)) found=1;
    fclose(f); return found;
}

static void testEngineUsesToken(void){
    printf("== C. sync_collection persists + uses token ==\n");
    const char*MAP="state/tok.map"; const char*PDB="pdb/tok.pdb";
    remove(MAP);
    freshColl();
    writeDB(PDB,3);
    SyncStats s1={0}; sync_collection(&D,PDB,PDB,COLL,1,MAP,POL_SERVER,&s1);
    CK(s1.pushNew==3,"seed pushes 3");
    CK(mapHasToken(MAP),"sync-token persisted to map header");

    SyncStats s2={0}; sync_collection(&D,PDB,PDB,COLL,1,MAP,POL_SERVER,&s2);
    int ops=s2.pushNew+s2.pushMod+s2.pushDel+s2.pullNew+s2.pullMod+s2.pullDel+s2.conflicts;
    printf("  2nd sync ops=%d clean=%d\n",ops,s2.unchanged);
    CK(ops==0,"delta-driven second sync is a no-op");
    CK(s2.unchanged==3,"all 3 clean");
}

int main(void){
    snprintf(D.base,sizeof D.base,"%s",getenv("DAV_BASE")?getenv("DAV_BASE"):"http://localhost:5232");
    snprintf(D.user,sizeof D.user,"palm"); snprintf(D.pass,sizeof D.pass,"palm");
    testReportDeltas();
    testInvalidToken();
    testEngineUsesToken();
    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
