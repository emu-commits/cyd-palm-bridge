/* bigsync.c -- prove the streaming engine lifts the old device cap.
 *
 * Compiled with -DSYNC_DEVICE_SIZES so sync.c uses the *device* working-set
 * sizing (MAXR=64) on the host. Before the streaming rewrite the device build
 * capped a collection at MAXR=24 records AND at an 8 KB record arena; this test
 * syncs 90 records each carrying a ~300-byte note (~27 KB of record bytes, well
 * past both old walls) and asserts they all sync, converge, and are idempotent.
 *
 * Needs Radicale on localhost:5232 with the palm/cal collection (run_gates.sh
 * sets that up). Uses its own map so it doesn't disturb the other gates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"

#define N 20
static DavCtx D;
static const char* COLL="palm/cal";
static const char* MAP ="state/big.map";
static const char* LPDB="pdb/big.pdb";
static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("  FAIL: %s\n",m); } }

/* build a DatebookDB of n events, each with a long note to inflate byte size */
static void writeDB(int n){
    uint8_t *arena = malloc((size_t)n*PALM_REC_MAX);
    PdbRec  *r     = calloc(n,sizeof *r);
    int used=0;
    for(int i=0;i<n;i++){
        Appt a; memset(&a,0,sizeof a);
        a.hasTime=1; a.sH=8+(i%10); a.eH=a.sH+1; a.year=2026; a.month=1+(i%12); a.day=1+(i%27);
        snprintf(a.description,sizeof a.description,"Event %03d",i);
        /* ~300-byte note so 90 records blow well past the old 8 KB arena */
        for(int j=0;j<290;j++) a.note[j]=(char)('a'+((i+j)%26));
        a.note[290]=0;
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l;
    }
    pdb_write(LPDB,"DatebookDB",0x44415441,0x64617465,r,n);
    free(arena); free(r);
}

/* count objects currently in the collection */
typedef struct { int n; } Cnt;
static void cntCb(const char*name,const char*etag,void*ctx){ (void)etag; if(strstr(name,".ics")) ((Cnt*)ctx)->n++; }
static int serverCount(void){ Cnt c={0}; dav_list(&D,COLL,cntCb,&c); return c.n; }

/* count records in the local PDB */
static int localCb(const PdbRec*r,int i,void*ctx){ (void)r;(void)i; (*(int*)ctx)++; return 0; }
static int localCount(void){ int n=0; pdb_read(LPDB,localCb,&n); return n; }

typedef struct { char n[256][256]; int c; } NL;
static void collectCb(const char*name,const char*etag,void*ctx){
    (void)etag; NL*x=ctx; if(strstr(name,".ics")&&x->c<256) snprintf(x->n[x->c++],256,"%s",name);
}
static void clearColl(void){
    NL *l=calloc(1,sizeof*l);
    dav_list(&D,COLL,collectCb,l);
    for(int i=0;i<l->c;i++) dav_delete(&D,COLL,l->n[i],NULL);
    remove(MAP);
    free(l);
}

int main(void){
    snprintf(D.base,sizeof D.base,"%s",getenv("DAV_BASE")?getenv("DAV_BASE"):"http://localhost:5232");
    snprintf(D.user,sizeof D.user,"palm"); snprintf(D.pass,sizeof D.pass,"palm");

    printf("== bigsync: %d records, device sizing (MAXR=24) ==\n",N);
    clearColl();
    writeDB(N);
    CK(localCount()==N,"local PDB seeded with N records");

    /* 1. push all N up */
    SyncStats s1={0};
    int rc=sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);
    printf("  first sync: rc=%d pushNew=%d (server=%d, local=%d)\n",rc,s1.pushNew,serverCount(),localCount());
    CK(rc==N,"sync kept all N records");
    CK(s1.pushNew==N,"all N records pushed (was capped at 24 before streaming)");
    CK(serverCount()==N,"server holds all N objects");
    CK(localCount()==N,"merged local PDB still has all N records");

    /* 2. idempotence: nothing changed -> no ops, all N clean (lazy re-read path) */
    SyncStats s2={0};
    sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s2);
    int ops=s2.pushNew+s2.pushMod+s2.pushDel+s2.pullNew+s2.pullMod+s2.pullDel+s2.conflicts;
    printf("  second sync: ops=%d clean=%d\n",ops,s2.unchanged);
    CK(ops==0,"second sync is a no-op");
    CK(s2.unchanged==N,"all N records clean on second sync");

    /* 3. a server-side add pulls back through the same large working set */
    Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=6; a.eH=7; a.year=2027; a.month=1; a.day=1;
    snprintf(a.description,sizeof a.description,"Server extra");
    char v[2048]; ical_emit(v,sizeof v,&a,900001);
    char obj[3000]; int on=snprintf(obj,sizeof obj,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//t//EN\r\n%sEND:VCALENDAR\r\n",v);
    FILE*f=fopen("state/.bbody","wb"); fwrite(obj,1,on,f); fclose(f);
    char etag[160]; int st=0; dav_put(&D,COLL,"900001.ics","text/calendar; charset=utf-8","state/.bbody",NULL,etag,sizeof etag,&st);
    SyncStats s3={0};
    sync_collection(&D,LPDB,LPDB,COLL,KIND_CAL,MAP,POL_SERVER,&s3);
    printf("  after server add: pullNew=%d local=%d\n",s3.pullNew,localCount());
    CK(s3.pullNew==1,"pulled the new server object");
    CK(localCount()==N+1,"local PDB grew to N+1");

    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
