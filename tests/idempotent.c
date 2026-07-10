/* idempotent.c -- prove sync converges (no dup, no data loss) under the two
 * real-iCloud behaviors that Radicale's happy path (uidmatch) does NOT exercise:
 *
 *   A. ETAG CHURN. iCloud reformats an object server-side after a PUT, so the
 *      etag a later REPORT returns differs from the one our PUT saw -- with the
 *      body logically unchanged. Sync must pull it AT MOST once and then converge
 *      to a no-op; it must never keep re-pulling or duplicate.
 *
 *   B. UNRESOLVABLE RELOCATION (the on-device duplication bug). iCloud relocates
 *      an object to a UUID href; when we then GET it to read its UID, the fetch
 *      buffer is too small (a photo-heavy vCard on the no-PSRAM device) so the UID
 *      can't be read. The OLD engine fell back to uidHash(href) -> a divergent
 *      identity -> the mapped record looked server-deleted AND the object looked
 *      brand-new = a duplicate + a lost local record. The fixed engine DEFERS the
 *      object and suppresses deletes: no dup, no loss, converges once the object
 *      is resolvable again.
 *
 * This binary is built with a deliberately tiny OBJ_FETCH_CAP (see the Makefile)
 * so a padded object overflows the buffer on the host, reproducing the device
 * truncation deterministically. Needs Radicale with palm/cal (tests/gate.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"

static DavCtx D;
static const char* COLL="palm/cal";
static const char* MAP ="state/idem.map";
static const char* LPDB="pdb/idem.pdb";
static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("   FAIL: %s\n",m); } }

typedef struct { char n[64][256]; int c; } NL;
static void nameCb(const char*name,const char*etag,void*ctx){ (void)etag; NL*x=ctx;
    if(strstr(name,".ics")&&x->c<64) snprintf(x->n[x->c++],256,"%s",name); }
static int serverCount(void){ NL l={0}; dav_list(&D,COLL,nameCb,&l); return l.c; }
static void firstName(char*out,int cap){
    NL l={0}; dav_list(&D,COLL,nameCb,&l); snprintf(out,cap,"%s", l.c? l.n[0] : ""); }
static void clearColl(void){
    NL l={0}; dav_list(&D,COLL,nameCb,&l);
    for(int i=0;i<l.c;i++) dav_delete(&D,COLL,l.n[i],NULL);
    remove(MAP); remove(LPDB);
}
static int countCb(const PdbRec*r,int i,void*c){ (void)r;(void)i; (*(int*)c)++; return 0; }
static int localCount(void){ int n=0; pdb_read(LPDB,countCb,&n); return n; }
static void writeOne(uint32_t uid,uint8_t attr,const char*sum,int h){
    Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=h; a.eH=h+1; a.year=2026; a.month=9; a.day=1;
    snprintf(a.description,sizeof a.description,"%s",sum);
    uint8_t buf[PALM_REC_MAX]; int l=ApptPack(buf,sizeof buf,&a);
    PdbRec r={ .attr=attr,.uniqueID=uid,.data=buf,.len=l };
    pdb_write(LPDB,"DatebookDB",0x44415441,0x64617465,&r,1);
}

/* re-PUT the same object under `newHref`, deleting `oldHref`. padKB>0 bloats the
 * object with a giant DESCRIPTION so it overflows a tiny fetch buffer. Simulates
 * an iCloud relocation; Radicale assigns a fresh etag on the PUT (etag churn). */
static void relocate(const char*oldHref,const char*newHref,int padKB){
    static char body[131072]; if(dav_get(&D,COLL,oldHref,body,sizeof body)<=0) return;
    if(padKB>0){
        char*end=strstr(body,"END:VEVENT");
        if(end){
            static char big[131072]; int off=(int)(end-body);
            memcpy(big,body,off);
            int j=off; j+=snprintf(big+j,sizeof big-j,"DESCRIPTION:");
            for(int i=0;i<padKB*1024 && j<(int)sizeof big-64;i++) big[j++]='x';
            j+=snprintf(big+j,sizeof big-j,"\r\n%s",end);
            big[j]=0; memcpy(body,big,j+1);
        }
    }
    FILE*f=fopen("state/.tb","wb"); fwrite(body,1,strlen(body),f); fclose(f);
    dav_delete(&D,COLL,oldHref,NULL);
    char etag[160]; int st=0;
    dav_put(&D,COLL,newHref,"text/calendar; charset=utf-8","state/.tb",NULL,etag,sizeof etag,&st);
}
/* PUT a fresh small VEVENT with an explicit UID + href (used to make an object
 * resolvable again after the huge/unreadable phase). */
static void serverPutRaw(const char*href,const char*uid,const char*sum,int h){
    char obj[2048];
    snprintf(obj,sizeof obj,
      "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//test//EN\r\n"
      "BEGIN:VEVENT\r\nUID:%s\r\nDTSTART:20260901T%02d0000\r\nDTEND:20260901T%02d0000\r\n"
      "SUMMARY:%s\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n", uid,h,h+1,sum);
    FILE*f=fopen("state/.tb","wb"); fwrite(obj,1,strlen(obj),f); fclose(f);
    char etag[160]; int st=0;
    dav_put(&D,COLL,href,"text/calendar; charset=utf-8","state/.tb",NULL,etag,sizeof etag,&st);
}

/* ================= A. etag churn (server reformat) ================= */
static void testEtagChurn(void){
    printf("== A. server-side etag churn converges (no re-pull loop, no dup) ==\n");
    clearColl();
    writeOne(1,0,"Alpha",10);
    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s0);
    CK(s0.pushNew==1,"seed pushes 1");

    /* iCloud reformats the object: same href, same content, fresh etag. */
    char nm[256]; firstName(nm,sizeof nm);
    relocate(nm,nm,0);                      /* re-PUT in place -> new etag */

    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s1);
    printf("   after reformat: pull ~%d | server=%d local=%d\n",s1.pullMod,serverCount(),localCount());
    CK(serverCount()==1,"server still 1 after reformat");
    CK(localCount()==1,"local still 1 after reformat");

    SyncStats s2={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s2);
    int ops=s2.pushNew+s2.pushMod+s2.pushDel+s2.pullNew+s2.pullMod+s2.pullDel+s2.conflicts;
    printf("   idempotent next sync ops=%d clean=%d\n",ops,s2.unchanged);
    CK(ops==0,"etag churn converges to a no-op");
    CK(serverCount()==1&&localCount()==1,"exactly one copy each side");
}

/* ============ B. unresolvable relocation: defer, don't duplicate ============ */
static void testUnresolvableReloc(void){
    printf("== B. relocation whose UID can't be read -> deferred, no dup/loss ==\n");
    clearColl();
    writeOne(2,0,"Beta",11);
    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s0);
    CK(s0.pushNew==1,"seed pushes 1");

    /* relocate to a UUID href AND bloat it past the tiny fetch buffer, so the
     * GET-for-UID in resolveServer truncates and the UID can't be parsed. */
    char nm[256]; firstName(nm,sizeof nm);
    relocate(nm,"reloc-huge.ics",64);       /* 64 KB NOTE >> OBJ_FETCH_CAP */
    CK(serverCount()==1,"still 1 object on server after relocation");

    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s1);
    printf("   deferred sync: pull +%d -%d | push -%d | server=%d local=%d\n",
        s1.pullNew,s1.pullDel,s1.pushDel,serverCount(),localCount());
    CK(s1.pullNew==0,"no phantom new record from the unreadable object");
    CK(s1.pushDel==0,"no spurious server delete");
    CK(s1.pullDel==0,"local record NOT dropped");
    CK(localCount()==1,"local still holds exactly 1 (no loss)");
    CK(serverCount()==1,"server still holds exactly 1 (no duplicate)");

    /* object becomes resolvable again (UID readable) -> converges cleanly.
     * uid=2's synthesized server UID is palm-2@cyd (what the emitter minted). */
    dav_delete(&D,COLL,"reloc-huge.ics",NULL);
    serverPutRaw("reloc-small.ics","palm-2@cyd","Beta",11);
    SyncStats s2={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s2);
    printf("   after object shrinks: server=%d local=%d\n",serverCount(),localCount());
    CK(localCount()==1,"local still 1 after resolvable");
    CK(serverCount()==1,"server still 1 after resolvable");

    SyncStats s3={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s3);
    int ops=s3.pushNew+s3.pushMod+s3.pushDel+s3.pullNew+s3.pullMod+s3.pullDel+s3.conflicts;
    printf("   idempotent final sync ops=%d clean=%d\n",ops,s3.unchanged);
    CK(ops==0,"converges to a no-op once resolvable");
}

int main(void){
    snprintf(D.base,sizeof D.base,"%s",getenv("DAV_BASE")?getenv("DAV_BASE"):"http://localhost:5232");
    snprintf(D.user,sizeof D.user,"palm"); snprintf(D.pass,sizeof D.pass,"palm");
    testEtagChurn();
    testUnresolvableReloc();
    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
