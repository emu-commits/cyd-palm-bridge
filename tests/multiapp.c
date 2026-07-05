/* multiapp.c -- prove sync_collection works for To Do (VTODO) and Address
 * (vCard), not just Date Book. HotSync now calls sync_collection once per app
 * (Date Book / To Do / Address), each against its own collection; the DateBook
 * path is covered by incremental.c, so this closes the gap for the other two.
 *
 * Per kind, against a freshly-cleared collection:
 *   1. seed records, sync -> all push, map written
 *   2. sync again with no edits -> total no-op (idempotent)
 *   3. add one on the server -> next sync pulls it into the local PDB
 *
 * Needs Radicale on localhost:5232 with palm/cal (holds VTODOs fine) + palm/card.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"

static DavCtx D;
static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("  FAIL: %s\n",m); } }

/* ---- clear a collection + its map ---- */
typedef struct { char n[128][256]; int c; } NL;
static void nlCb(const char*name,const char*etag,void*ctx){
    (void)etag; NL*l=ctx; if((strstr(name,".ics")||strstr(name,".vcf"))&&l->c<128) snprintf(l->n[l->c++],256,"%s",name);
}
static void clearColl(const char*coll,const char*map){
    NL *l=calloc(1,sizeof*l); dav_list(&D,coll,nlCb,l);
    for(int i=0;i<l->c;i++) dav_delete(&D,coll,l->n[i],NULL);
    remove(map); free(l);
}
static int collCount(const char*coll){ NL *l=calloc(1,sizeof*l); dav_list(&D,coll,nlCb,l); int n=l->c; free(l); return n; }
static int countCb(const PdbRec*r,int i,void*c){ (void)r;(void)i; (*(int*)c)++; return 0; }
static int localCount(const char*pdb){ int n=0; pdb_read(pdb,countCb,&n); return n; }

/* ================= To Do ================= */
static void buildTodoDB(const char*path,int n){
    uint8_t arena[16*PALM_REC_MAX]; PdbRec r[16]; int used=0;
    for(int i=0;i<n;i++){ Todo t; memset(&t,0,sizeof t);
        t.priority=1+(i%5); t.completed=0; t.hasDue=1; t.dueY=2026; t.dueM=1+(i%12); t.dueD=1+i;
        snprintf(t.description,sizeof t.description,"Task %d",i);
        uint8_t*dst=arena+used; int l=ToDoPack(dst,PALM_REC_MAX,&t);
        r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l; }
    pdb_write(path,"ToDoDB",0x44415441,0x746F646F,r,n);
}
static void serverPutTodo(const char*coll,const char*name,const char*desc){
    Todo t; memset(&t,0,sizeof t); t.priority=2; t.hasDue=1; t.dueY=2026; t.dueM=6; t.dueD=15;
    snprintf(t.description,sizeof t.description,"%s",desc);
    char v[2048]; vtodo_emit(v,sizeof v,&t,777);
    char obj[3000]; int on=snprintf(obj,sizeof obj,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//t//EN\r\n%sEND:VCALENDAR\r\n",v);
    FILE*f=fopen("state/.mbody","wb"); fwrite(obj,1,on,f); fclose(f);
    char etag[160]; int st=0; dav_put(&D,coll,name,"text/calendar; charset=utf-8","state/.mbody",NULL,etag,sizeof etag,&st);
}

/* ================= Address ================= */
static void buildAddrDB(const char*path,int n){
    uint8_t arena[16*PALM_REC_MAX]; PdbRec r[16]; int used=0;
    for(int i=0;i<n;i++){ Addr a; memset(&a,0,sizeof a);
        char nm[32]; snprintf(nm,sizeof nm,"Last%d",i); a.fields[F_name]=AddrIntern(&a,nm);
        a.fields[F_firstName]=AddrIntern(&a,"First");
        a.fields[F_company]=AddrIntern(&a,"Acme");
        char ph[32]; snprintf(ph,sizeof ph,"555-000%d",i); a.fields[F_phone1]=AddrIntern(&a,ph);
        a.phoneLabel[0]=workLabel;
        uint8_t*dst=arena+used; int l=AddrPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l; }
    pdb_write(path,"AddressDB",0x44415441,0x61646472,r,n);
}
static void serverPutCard(const char*coll,const char*name,const char*last){
    Addr a; memset(&a,0,sizeof a);
    a.fields[F_name]=AddrIntern(&a,last); a.fields[F_firstName]=AddrIntern(&a,"Srv");
    char v[2048]; vcard_emit(v,sizeof v,&a,888);
    FILE*f=fopen("state/.mbody","wb"); fwrite(v,1,strlen(v),f); fclose(f);
    char etag[160]; int st=0; dav_put(&D,coll,name,"text/vcard; charset=utf-8","state/.mbody",NULL,etag,sizeof etag,&st);
}

static void run(const char*label,int kind,const char*coll,const char*pdb,const char*map){
    printf("== %s (sync_collection, kind=%d) ==\n",label,kind);
    clearColl(coll,map);
    if(kind==KIND_TODO) buildTodoDB(pdb,4); else buildAddrDB(pdb,4);
    CK(localCount(pdb)==4,"seeded 4 records");

    SyncStats s1={0}; int rc=sync_collection(&D,pdb,pdb,coll,kind,map,POL_SERVER,&s1);
    printf("  push: rc=%d new=%d (server=%d)\n",rc,s1.pushNew,collCount(coll));
    CK(s1.pushNew==4,"all 4 pushed");
    CK(collCount(coll)==4,"server holds 4 objects");

    SyncStats s2={0}; sync_collection(&D,pdb,pdb,coll,kind,map,POL_SERVER,&s2);
    int ops=s2.pushNew+s2.pushMod+s2.pushDel+s2.pullNew+s2.pullMod+s2.pullDel+s2.conflicts;
    printf("  idempotent: ops=%d clean=%d\n",ops,s2.unchanged);
    CK(ops==0,"second sync is a no-op");
    CK(s2.unchanged==4,"all 4 clean on second sync");

    if(kind==KIND_TODO) serverPutTodo(coll,"srv1.ics","Server task");
    else                serverPutCard(coll,"srv1.vcf","ServerContact");
    SyncStats s3={0}; sync_collection(&D,pdb,pdb,coll,kind,map,POL_SERVER,&s3);
    printf("  server add: pullNew=%d local=%d\n",s3.pullNew,localCount(pdb));
    CK(s3.pullNew==1,"pulled the new server object");
    CK(localCount(pdb)==5,"local PDB grew to 5");
}

int main(void){
    snprintf(D.base,sizeof D.base,"%s",getenv("DAV_BASE")?getenv("DAV_BASE"):"http://localhost:5232");
    snprintf(D.user,sizeof D.user,"palm"); snprintf(D.pass,sizeof D.pass,"palm");
    run("To Do",   KIND_TODO, "palm/cal",  "pdb/ma_todo.pdb", "state/ma_todo.map");
    run("Address", KIND_CARD, "palm/card", "pdb/ma_addr.pdb", "state/ma_card.map");
    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
