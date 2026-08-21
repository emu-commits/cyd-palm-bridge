/* toobig.c -- prove the engine REFUSES a collection it cannot process, instead
 * of quietly producing a wrong one.
 *
 * The two ways a big collection used to break were both silent, and both worse
 * than a crash:
 *
 *   sortFile() returned with the file UNSORTED on a failed malloc, and the
 *   three-way merge-join then walked it as if sorted -- mis-pairing records
 *   into spurious deletes and duplicates against the real account.
 *
 *   pdbw_rec()'s return was never checked, so records were dropped from the
 *   merged PDB; the NEXT sync read them as locally deleted and pushed those
 *   deletions to the server.
 *
 * A real iCloud calendar with years of history reaches those limits easily
 * (SV_RAW is ~60-80 bytes per record), so the failure has to be safe: refuse,
 * change nothing locally, do not republish the map, and say so.
 *
 * sync_set_max_sort() makes the ceiling a decision rather than a discovery,
 * which is also what lets this gate fire it without staging a real OOM.
 *
 * Needs Radicale on localhost:5232 with palm/cal.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"

static DavCtx D;
static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("  FAIL: %s\n",m); } }

#define PDB  "pdb/tb_cal.pdb"
#define MAP  "state/tb_cal.map"
#define COLL "palm/cal"
#define N    12

typedef struct { char n[256][256]; int c; } NL;
static void nlCb(const char*name,const char*etag,void*ctx){
    (void)etag; NL*l=ctx; if(strstr(name,".ics") && l->c<256) snprintf(l->n[l->c++],256,"%s",name);
}
static void clearColl(void){
    NL *l=calloc(1,sizeof*l); dav_list(&D,COLL,nlCb,l);
    for(int i=0;i<l->c;i++) dav_delete(&D,COLL,l->n[i],NULL);
    remove(MAP); free(l);
}
static int serverCount(void){ NL *l=calloc(1,sizeof*l); dav_list(&D,COLL,nlCb,l); int n=l->c; free(l); return n; }
static int countCb(const PdbRec*r,int i,void*c){ (void)r;(void)i; (*(int*)c)++; return 0; }
static int localCount(const char*p){ int n=0; pdb_read(p,countCb,&n); return n; }
static int fileExists(const char*p){ FILE*f=fopen(p,"rb"); if(f){fclose(f);return 1;} return 0; }

static void buildCal(int n){
    static uint8_t arena[N*PALM_REC_MAX]; static PdbRec r[N]; int used=0;
    for(int i=0;i<n;i++){ Appt a; memset(&a,0,sizeof a);
        a.year=2026; a.month=1+(i%12); a.day=1+(i%28); a.hasTime=1; a.sH=9; a.eH=10;
        snprintf(a.description,sizeof a.description,"Event %d",i);
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l; }
    pdb_write(PDB,"DatebookDB",0x44415441,0x64617465,r,n);
}

int main(void){
    snprintf(D.base,sizeof D.base,"%s",getenv("DAV_BASE")?getenv("DAV_BASE"):"http://localhost:5232");
    snprintf(D.user,sizeof D.user,"palm"); snprintf(D.pass,sizeof D.pass,"palm");

    printf("== a collection larger than the device can sort ==\n");
    clearColl();
    buildCal(N);
    sync_set_max_sort(0);                       /* no ceiling: establish a good state */
    SyncStats s0={0};
    int rc0 = sync_collection(&D,PDB,PDB,COLL,KIND_CAL,MAP,POL_SERVER,&s0);
    CK(rc0==N, "baseline: the collection syncs normally");
    CK(serverCount()==N, "baseline: every record reached the server");
    CK(fileExists(MAP), "baseline: the map was written");

    /* Snapshot what must not change when the refusal happens. */
    int locBefore = localCount(PDB), srvBefore = serverCount();
    FILE *mf = fopen(MAP,"rb"); long mapBefore = 0;
    if(mf){ fseek(mf,0,SEEK_END); mapBefore = ftell(mf); fclose(mf); }

    /* Now make any sort of consequence impossible. 16 bytes is smaller than any
     * real index file, so the first sortFile refuses. */
    sync_set_max_sort(16);
    SyncStats s1={0};
    int rc1 = sync_collection(&D,PDB,PDB,COLL,KIND_CAL,MAP,POL_SERVER,&s1);

    CK(rc1==-6, "refused with -6 (too large for this device), not a partial success");
    CK(rc1!=0,  "and NOT reported as a clean sync");
    CK(sync_too_big_bytes()>0, "the size it could not get is reported");

    /* The whole point: nothing moved. */
    CK(localCount(PDB)==locBefore, "the local PDB is byte-for-byte untouched in record count");
    CK(serverCount()==srvBefore,   "NOTHING was deleted from the server");
    CK(s1.pushDel==0,              "no deletions were even attempted");
    CK(s1.pushNew==0 && s1.pushMod==0, "no writes were attempted");
    CK(s1.pullNew==0 && s1.pullMod==0, "no pulls were applied");
    mf = fopen(MAP,"rb"); long mapAfter = 0;
    if(mf){ fseek(mf,0,SEEK_END); mapAfter = ftell(mf); fclose(mf); }
    CK(mapAfter==mapBefore, "the map was NOT republished (next run reconciles from the real state)");
    CK(!fileExists(MAP ".tmp"), "the half-written map temp was cleaned up");

    /* And the refusal is not sticky: lift the ceiling and it works again. */
    sync_set_max_sort(0);
    SyncStats s2={0};
    int rc2 = sync_collection(&D,PDB,PDB,COLL,KIND_CAL,MAP,POL_SERVER,&s2);
    CK(rc2==N, "with room again it syncs normally -- the refusal left no damage");
    CK(serverCount()==srvBefore, "and still nothing was lost on the server");

    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
