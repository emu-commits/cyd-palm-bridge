/* category.c -- prove category->collection routing against Radicale.
 *
 * A DatebookDB with an AppInfo category table (1=Business, 2=Personal) and
 * records in categories 1/2/0 is synced with a CatRoute mapping each category
 * to its own calendar collection. We then assert:
 *   - each record landed in the right collection (partitioning)
 *   - the merged local PDB preserves each record's category nibble
 *   - a server-side add in the Business calendar pulls back tagged Business
 *   - a second sync is a no-op (idempotent across all collections)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/appinfo.h"
#include "../bridge/sync.h"

static DavCtx D;
static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("  FAIL: %s\n",m);} }

static const char* BIZ="palm/cat_biz";
static const char* PER="palm/cat_per";
static const char* DEF="palm/cat_def";

static void fresh(const char*coll){
    char cmd[512];
    snprintf(cmd,sizeof cmd,"curl -s -o /dev/null -u %s:%s -X DELETE '%s/%s/'",D.user,D.pass,D.base,coll); if(system(cmd)){}
    snprintf(cmd,sizeof cmd,"curl -s -o /dev/null -u %s:%s -X MKCALENDAR '%s/%s/'",D.user,D.pass,D.base,coll); if(system(cmd)){}
}

/* list object names in a collection, then GET each */
typedef struct { char n[64][128]; int c; } NL;
static void nlCb(const char*name,const char*etag,void*ctx){ (void)etag; NL*l=ctx; if(strstr(name,".ics")&&l->c<64) snprintf(l->n[l->c++],128,"%s",name); }
static int collHasSummary(const char*coll,const char*want){
    NL l={0}; dav_list(&D,coll,nlCb,&l);
    for(int i=0;i<l.c;i++){ char obj[16384]; if(dav_get(&D,coll,l.n[i],obj,sizeof obj)<=0) continue;
        Appt a; if(ical_parse(obj,&a)) continue; if(!strcmp(a.description,want)) return 1; }
    return 0;
}
static int collCount(const char*coll){ NL l={0}; dav_list(&D,coll,nlCb,&l); return l.c; }

/* read local PDB: category of the record whose summary==want, or -1 */
typedef struct { const char*want; int cat; } Find;
static int findCb(const PdbRec*r,int i,void*ctx){ (void)i; Find*f=ctx;
    Appt a; if(ApptUnpack(r->data,r->len,&a)) return 0;
    if(!strcmp(a.description,f->want)) f->cat = r->attr & REC_ATTR_CAT;
    return 0; }
static int localCat(const char*pdb,const char*want){ Find f={want,-1}; pdb_read(pdb,findCb,&f); return f.cat; }
static int localHas(const char*pdb,const char*want){ return localCat(pdb,want)>=0; }

static void buildDB(const char*path){
    CatTable t; memset(&t,0,sizeof t);
    strcpy(t.name[0],"Unfiled"); strcpy(t.name[1],"Business"); strcpy(t.name[2],"Personal");
    uint8_t ai[APPINFO_SIZE]; int al=appinfo_build(ai,sizeof ai,&t);
    struct { const char*sum; int cat; } specs[]={ {"Board meeting",1},{"Dentist",2},{"Misc",0} };
    static uint8_t arena[8*PALM_REC_MAX]; PdbRec r[8]; int used=0;
    for(int i=0;i<3;i++){ Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=9;a.eH=10;
        a.year=2026;a.month=10;a.day=1+i; snprintf(a.description,sizeof a.description,"%s",specs[i].sum);
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=(uint8_t)specs[i].cat,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l; }
    pdb_write_ai(path,"DatebookDB",0x44415441,0x64617465,ai,al,r,3);
}

static void serverPut(const char*coll,const char*name,const char*sum){
    Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=11;a.eH=12; a.year=2026;a.month=10;a.day=20;
    snprintf(a.description,sizeof a.description,"%s",sum);
    char v[2048]; ical_emit(v,sizeof v,&a,999);
    char obj[3000]; int n=snprintf(obj,sizeof obj,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//t//EN\r\n%sEND:VCALENDAR\r\n",v);
    FILE*f=fopen("state/.cput","wb"); fwrite(obj,1,n,f); fclose(f);
    char etag[160]; int st=0; dav_put(&D,coll,name,"text/calendar; charset=utf-8","state/.cput",NULL,etag,sizeof etag,&st);
}

int main(void){
    snprintf(D.base,sizeof D.base,"%s",getenv("DAV_BASE")?getenv("DAV_BASE"):"http://localhost:5232");
    snprintf(D.user,sizeof D.user,"palm"); snprintf(D.pass,sizeof D.pass,"palm");

    printf("== category routing ==\n");
    fresh(BIZ); fresh(PER); fresh(DEF);
    /* clear THIS test's per-collection maps. sync_categorized names them after
     * the sanitized collection path (slash/colon -> '_'), i.e. palm_cat_biz.map,
     * not cat_biz.map -- the old glob never matched, so stale maps from a prior
     * run leaked in and made back-to-back runs flaky. Remove the real names. */
    if(system("rm -f state/palm_cat_biz.map state/palm_cat_per.map state/palm_cat_def.map")){}
    const char*PDB="pdb/cat.pdb"; buildDB(PDB);

    CatRoute rt; memset(&rt,0,sizeof rt);
    rt.coll[1]=BIZ; rt.coll[2]=PER; rt.def=DEF;

    SyncStats s1={0}; sync_categorized(&D,PDB,PDB,KIND_CAL,&rt,"state",POL_SERVER,&s1);
    printf("  push: new=%d\n",s1.pushNew);
    CK(s1.pushNew==3,"3 records pushed across collections");
    CK(collHasSummary(BIZ,"Board meeting"),"Business calendar has the Business event");
    CK(collHasSummary(PER,"Dentist"),"Personal calendar has the Personal event");
    CK(collHasSummary(DEF,"Misc"),"Default calendar has the Unfiled event");
    CK(collCount(BIZ)==1&&collCount(PER)==1&&collCount(DEF)==1,"one object per collection");

    /* local PDB preserves each record's category nibble */
    CK(localCat(PDB,"Board meeting")==1,"Board meeting kept category 1");
    CK(localCat(PDB,"Dentist")==2,"Dentist kept category 2");
    CK(localCat(PDB,"Misc")==0,"Misc kept category 0");

    /* server-side add in Business -> pulled back tagged Business */
    serverPut(BIZ,"srvbiz.ics","New biz task");
    SyncStats s2={0}; sync_categorized(&D,PDB,PDB,KIND_CAL,&rt,"state",POL_SERVER,&s2);
    printf("  after server add: pullNew=%d\n",s2.pullNew);
    CK(s2.pullNew==1,"pulled the new server object");
    CK(localHas(PDB,"New biz task"),"new server object is in the local PDB");
    CK(localCat(PDB,"New biz task")==1,"pulled object stamped category 1 (Business)");

    /* idempotent third sync */
    SyncStats s3={0}; sync_categorized(&D,PDB,PDB,KIND_CAL,&rt,"state",POL_SERVER,&s3);
    int ops=s3.pushNew+s3.pushMod+s3.pushDel+s3.pullNew+s3.pullMod+s3.pullDel+s3.conflicts;
    printf("  3rd sync ops=%d clean=%d\n",ops,s3.unchanged);
    CK(ops==0,"third sync is a no-op across all collections");

    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
