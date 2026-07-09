/* uidmatch.c -- prove reconciliation is anchored on the object UID, not the href.
 *
 * Two failure modes that the href-derived-identity engine got wrong, and that
 * UID matching fixes:
 *
 *   A. RELOCATION. We push a record (href <uid>.ics). iCloud later moves the SAME
 *      object (same UID) to a different href and drops the old one -- routine on
 *      cross-device edits. The old engine saw "my mapped href vanished" (spurious
 *      server-delete) AND "a new href appeared" (a duplicate record). UID matching
 *      recognizes it as the same record: no delete, no dup, map follows the href.
 *
 *   B. FOREIGN EDIT ROUND-TRIP. An object created by another client (its own,
 *      non-palm UID, at a UUID-style href) is pulled locally. We edit it and push:
 *      the update must go back to the SAME href carrying the SAME UID (CalDAV
 *      treats UID as immutable), not create a fresh <palmuid>.ics duplicate.
 *
 * Needs Radicale with palm/cal (tests/gate.sh sets that up).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"

static DavCtx D;
static const char* COLL="palm/cal";
static const char* MAP ="state/uidm.map";
static const char* LPDB="pdb/uidm.pdb";
static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("   FAIL: %s\n",m); } }

/* ---- server-side helpers (simulate other clients) ---- */
typedef struct { char n[64][256]; int c; } NL;
static void nameCb(const char*name,const char*etag,void*ctx){ (void)etag; NL*x=ctx;
    if(strstr(name,".ics")&&x->c<64) snprintf(x->n[x->c++],256,"%s",name); }
static int serverCount(void){ NL l={0}; dav_list(&D,COLL,nameCb,&l); return l.c; }
static void firstName(char*out,int cap){
    NL l={0}; dav_list(&D,COLL,nameCb,&l);
    snprintf(out,cap,"%s", l.c? l.n[0] : "");
}
static void clearColl(void){
    NL l={0}; dav_list(&D,COLL,nameCb,&l);
    for(int i=0;i<l.c;i++) dav_delete(&D,COLL,l.n[i],NULL);
    remove(MAP); remove(LPDB);              /* start each test from empty state */
}

/* PUT a raw VEVENT with an arbitrary UID + href (an object we did not author). */
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
/* pull the UID: value out of a fetched object. */
static int getUid(const char*href,char*out,int cap){
    char obj[16384]; if(dav_get(&D,COLL,href,obj,sizeof obj)<=0) return -1;
    const char*p=obj;
    while((p=strstr(p,"UID:"))){
        if(p==obj||p[-1]=='\n'){
            p+=4; int j=0;
            while(*p&&*p!='\r'&&*p!='\n'&&j<cap-1) out[j++]=*p++;
            out[j]=0; return 0;
        }
        p+=4;
    }
    return -1;
}
static int getSummary(const char*href,char*out,int cap){
    char obj[16384]; if(dav_get(&D,COLL,href,obj,sizeof obj)<=0) return -1;
    Appt a; if(ical_parse(obj,&a)) return -1; snprintf(out,cap,"%s",a.description); return 0;
}

/* ---- local PDB helpers ---- */
static void writeOne(uint32_t uid,uint8_t attr,const char*sum,int h){
    Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=h; a.eH=h+1; a.year=2026; a.month=9; a.day=1;
    snprintf(a.description,sizeof a.description,"%s",sum);
    uint8_t buf[PALM_REC_MAX]; int l=ApptPack(buf,sizeof buf,&a);
    PdbRec r={ .attr=attr,.uniqueID=uid,.data=buf,.len=l };
    pdb_write(LPDB,"DatebookDB",0x44415441,0x64617465,&r,1);
}
typedef struct { uint32_t uid; uint8_t attr; char sum[256]; int has; } One;
static int grab(const PdbRec*r,int i,void*ctx){ (void)i; One*o=ctx; if(o->has)return 0;
    Appt a; if(ApptUnpack(r->data,r->len,&a))return 0;
    o->uid=r->uniqueID; o->attr=r->attr; snprintf(o->sum,sizeof o->sum,"%s",a.description); o->has=1; return 0; }
static int countCb(const PdbRec*r,int i,void*c){ (void)r;(void)i; (*(int*)c)++; return 0; }
static int localCount(void){ int n=0; pdb_read(LPDB,countCb,&n); return n; }

/* ================= A. relocation ================= */
static void testRelocation(void){
    printf("== A. href relocation keeps one record ==\n");
    clearColl();
    writeOne(1,0,"Alpha",10);
    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s0);
    CK(s0.pushNew==1,"seed pushes 1");
    CK(serverCount()==1,"server has 1 object");

    /* iCloud moves the object to a new href. Delete the old first: the server
     * (like iCloud) enforces one-UID-per-collection, so the copy can't coexist. */
    char old[256]; firstName(old,sizeof old);
    char body[16384]; CK(dav_get(&D,COLL,old,body,sizeof body)>0,"fetch original body");
    FILE*f=fopen("state/.tb","wb"); fwrite(body,1,strlen(body),f); fclose(f);
    dav_delete(&D,COLL,old,NULL);
    char etag[160]; int st=0;
    dav_put(&D,COLL,"relocated-A.ics","text/calendar; charset=utf-8","state/.tb",NULL,etag,sizeof etag,&st);
    CK(serverCount()==1,"still 1 object after relocation");

    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s1);
    printf("   after relocation: push +%d -%d | pull +%d ~%d -%d | conflict %d\n",
        s1.pushNew,s1.pushDel,s1.pullNew,s1.pullMod,s1.pullDel,s1.conflicts);
    CK(s1.pushDel==0,"no spurious server-delete");
    CK(s1.pullNew==0,"no phantom new record");
    CK(serverCount()==1,"server still holds exactly 1 (no duplicate)");
    CK(localCount()==1,"local still holds exactly 1");
    One o={0}; pdb_read(LPDB,grab,&o); CK(o.has&&!strcmp(o.sum,"Alpha"),"local record intact");

    SyncStats s2={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s2);
    int ops=s2.pushNew+s2.pushMod+s2.pushDel+s2.pullNew+s2.pullMod+s2.pullDel+s2.conflicts;
    printf("   idempotent 2nd sync ops=%d clean=%d\n",ops,s2.unchanged);
    CK(ops==0,"relocation converges: 2nd sync is a no-op");
}

/* ============= B. foreign edit round-trip ============= */
static void testForeignEdit(void){
    printf("== B. foreign object edit round-trips to its own UID/href ==\n");
    clearColl();
    const char*FUID="9C1B2D34-FOREIGN-5E6F";      /* a UID we did not mint     */
    const char*FHREF="AB12CD34-EF56.ics";          /* a UUID-style href         */
    serverPutRaw(FHREF,FUID,"MadeElsewhere",14);

    SyncStats s0={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s0);
    CK(s0.pullNew==1,"pulled the foreign object");
    CK(localCount()==1,"one local record after pull");

    /* edit the pulled record locally (keep its assigned palm uid) */
    One o={0}; pdb_read(LPDB,grab,&o); CK(o.has,"grabbed pulled record");
    writeOne(o.uid,0,"EditedLocally",14);

    SyncStats s1={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s1);
    printf("   after edit: push ~%d +%d | conflict %d | server=%d\n",
        s1.pushMod,s1.pushNew,s1.conflicts,serverCount());
    CK(s1.pushMod==1,"the edit is a push-mod");
    CK(s1.pushNew==0,"no new object created");
    CK(s1.conflicts==0,"no conflict/dup");
    CK(serverCount()==1,"server still holds exactly 1 (pushed in place, no dup)");

    char nm[256]; firstName(nm,sizeof nm);
    CK(!strcmp(nm,FHREF),"update landed on the ORIGINAL href");
    char uid[128]=""; getUid(nm,uid,sizeof uid);
    CK(!strcmp(uid,FUID),"update preserved the foreign UID");
    char sum[256]=""; getSummary(nm,sum,sizeof sum);
    CK(!strcmp(sum,"EditedLocally"),"server object carries the edit");

    SyncStats s2={0}; sync_collection(&D,LPDB,LPDB,COLL,1,MAP,POL_SERVER,&s2);
    int ops=s2.pushNew+s2.pushMod+s2.pushDel+s2.pullNew+s2.pullMod+s2.pullDel+s2.conflicts;
    printf("   idempotent 2nd sync ops=%d clean=%d\n",ops,s2.unchanged);
    CK(ops==0,"foreign edit converges: 2nd sync is a no-op");
}

int main(void){
    snprintf(D.base,sizeof D.base,"%s",getenv("DAV_BASE")?getenv("DAV_BASE"):"http://localhost:5232");
    snprintf(D.user,sizeof D.user,"palm"); snprintf(D.pass,sizeof D.pass,"palm");
    testRelocation();
    testForeignEdit();
    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
