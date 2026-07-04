/* roundtrip.c -- prove the codec round-trip end to end, no server needed.
 *
 * For DateBook and Address:
 *   L1  PDB codec lossless:   Appt -> pack -> pdb_write -> pdb_read -> unpack
 *                             must equal the original on EVERY field.
 *   L2  DAV representation:   -> emit(ICS/vCard) -> parse -> equal on the
 *                             subset a DAV object can carry.
 *   L3  full bridge chain:    parsed-from-DAV -> pack -> pdb_write -> pdb_read
 *                             -> unpack must still equal the original subset.
 *                             (this is the "server rebuilt a valid PDB" proof)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../bridge/palm.h"
#include "../bridge/appinfo.h"

static int fails=0, checks=0;
#define CK(cond,msg) do{ checks++; if(!(cond)){ fails++; printf("  FAIL: %s\n",msg);} }while(0)
#define CKSTR(x,y,msg) CK(!strcmp((x)?(x):"",(y)?(y):""),msg)

/* ---------- collectors: capture records streamed out of pdb_read ---------- */
typedef struct { Appt a[8]; int n; } ApptBag;
static int apptCollect(const PdbRec *r,int i,void *ctx){ (void)i; ApptBag*b=ctx;
    if(b->n<8 && ApptUnpack(r->data,r->len,&b->a[b->n])==0) b->n++;
    return 0; }
typedef struct { Addr a[8]; int n; } AddrBag;
static int addrCollect(const PdbRec *r,int i,void *ctx){ (void)i; AddrBag*b=ctx;
    if(b->n<8 && AddrUnpack(r->data,r->len,&b->a[b->n])==0) b->n++;
    return 0; }
static int catNibble(const PdbRec *r,int i,void *ctx){ (void)i; *(int*)ctx = r->attr & REC_ATTR_CAT; return 0; }

/* ---------- semantic compares ---------- */
static void cmpAppt_full(const Appt*x,const Appt*y,const char*tag){
    printf(" [%s] full/PDB compare\n",tag);
    CK(x->hasTime==y->hasTime,"hasTime");
    CK(x->year==y->year&&x->month==y->month&&x->day==y->day,"date");
    if(x->hasTime) CK(x->sH==y->sH&&x->sM==y->sM&&x->eH==y->eH&&x->eM==y->eM,"time");
    CK(x->hasAlarm==y->hasAlarm,"hasAlarm");
    if(x->hasAlarm) CK(x->alarmAdv==y->alarmAdv&&x->alarmUnit==y->alarmUnit,"alarm value");
    CK(x->hasRepeat==y->hasRepeat,"hasRepeat");
    if(x->hasRepeat){
        CK(x->repeatType==y->repeatType,"repeatType");
        CK(x->repeatFreq==y->repeatFreq,"repeatFreq");
        CK(x->repeatOn==y->repeatOn,"repeatOn");
        CK(x->repeatForever==y->repeatForever,"repeatForever");
        if(!x->repeatForever) CK(x->endYear==y->endYear&&x->endMonth==y->endMonth&&x->endDay==y->endDay,"until");
    }
    CK(x->nExcept==y->nExcept,"nExcept");
    for(int i=0;i<x->nExcept;i++) CK(x->excpt[i].y==y->excpt[i].y&&x->excpt[i].m==y->excpt[i].m&&x->excpt[i].d==y->excpt[i].d,"exception date");
    CKSTR(x->description,y->description,"description");
    CKSTR(x->note,y->note,"note");
}
static void cmpAppt_dav(const Appt*x,const Appt*y,const char*tag){
    printf(" [%s] DAV-subset compare (now incl. VALARM + EXDATE)\n",tag);
    CK(x->hasTime==y->hasTime,"hasTime");
    CK(x->year==y->year&&x->month==y->month&&x->day==y->day,"date");
    if(x->hasTime) CK(x->sH==y->sH&&x->sM==y->sM&&x->eH==y->eH&&x->eM==y->eM,"time");
    CK(x->hasRepeat==y->hasRepeat,"hasRepeat");
    if(x->hasRepeat){
        CK(x->repeatType==y->repeatType,"repeatType");
        CK(x->repeatFreq==y->repeatFreq,"repeatFreq");
        CK(x->repeatOn==y->repeatOn,"repeatOn");
        CK(x->repeatForever==y->repeatForever,"repeatForever");
        if(!x->repeatForever) CK(x->endYear==y->endYear&&x->endMonth==y->endMonth&&x->endDay==y->endDay,"until");
    }
    CK(x->hasAlarm==y->hasAlarm,"hasAlarm(ics)");
    if(x->hasAlarm) CK(x->alarmAdv==y->alarmAdv&&x->alarmUnit==y->alarmUnit,"alarm value(ics)");
    CK(x->nExcept==y->nExcept,"nExcept(ics)");
    for(int i=0;i<x->nExcept;i++) CK(x->excpt[i].y==y->excpt[i].y&&x->excpt[i].m==y->excpt[i].m&&x->excpt[i].d==y->excpt[i].d,"EXDATE");
    CKSTR(x->description,y->description,"description");
    CKSTR(x->note,y->note,"note");
}
static void cmpAddr(const Addr*x,const Addr*y,const char*tag,int withDisplay){
    printf(" [%s] Address compare\n",tag);
    for(int i=0;i<F_COUNT;i++) CKSTR(x->fields[i],y->fields[i],"field");
    for(int i=0;i<5;i++) if(x->fields[F_phone1+i]) CK(x->phoneLabel[i]==y->phoneLabel[i],"phoneLabel");
    if(withDisplay) CK(x->displayPhone==y->displayPhone,"displayPhone");
}

/* ---------- builders ---------- */
static Appt mkAppt1(void){ Appt a; memset(&a,0,sizeof a);
    a.hasTime=1; a.sH=14;a.sM=0;a.eH=15;a.eM=0; a.year=2026;a.month=7;a.day=2;
    a.hasAlarm=1; a.alarmAdv=10; a.alarmUnit=0;   /* 10 minutes before */
    /* description carries CP1252 bytes (e-acute, curly quotes, ellipsis) + escapables */
    strcpy(a.description,"Caf\xE9 \x91plan\x92 \x85 floss; bring card\\ok");
    strcpy(a.note,"line1\nline2"); return a; }
static Appt mkAppt2(void){ Appt a; memset(&a,0,sizeof a);
    a.hasTime=0; a.year=2026;a.month=7;a.day=4; strcpy(a.description,"Independence Day"); return a; }
static Appt mkAppt3(void){ Appt a; memset(&a,0,sizeof a);
    a.hasTime=1; a.sH=9;a.sM=30;a.eH=10;a.eM=0; a.year=2026;a.month=7;a.day=6;
    a.hasRepeat=1; a.repeatType=repeatWeekly; a.repeatFreq=2; a.repeatOn=(1<<1)|(1<<3);
    a.repeatForever=0; a.endYear=2026;a.endMonth=12;a.endDay=31; a.startOfWeek=0;
    a.nExcept=2; a.excpt[0]=(typeof(a.excpt[0])){2026,7,8}; a.excpt[1]=(typeof(a.excpt[0])){2026,7,15};
    strcpy(a.description,"Team standup"); return a; }

static void mkAddr1(Addr*a){ memset(a,0,sizeof*a);
    a->fields[F_name]=AddrIntern(a,"Smith"); a->fields[F_firstName]=AddrIntern(a,"John");
    a->fields[F_company]=AddrIntern(a,"Acme Corp"); a->fields[F_title]=AddrIntern(a,"Engineer");
    a->fields[F_phone1]=AddrIntern(a,"555-1000"); a->phoneLabel[0]=workLabel;
    a->fields[F_phone2]=AddrIntern(a,"555-2000"); a->phoneLabel[1]=mobileLabel;
    a->fields[F_phone3]=AddrIntern(a,"john@acme.com"); a->phoneLabel[2]=emailLabel;
    a->fields[F_address]=AddrIntern(a,"1 Main St"); a->fields[F_city]=AddrIntern(a,"Portland");
    a->fields[F_state]=AddrIntern(a,"OR"); a->fields[F_zip]=AddrIntern(a,"97201");
    a->fields[F_note]=AddrIntern(a,"met at conference"); a->displayPhone=0; }
static void mkAddr2(Addr*a){ memset(a,0,sizeof*a);
    a->fields[F_company]=AddrIntern(a,"Widgets Inc");
    a->fields[F_phone1]=AddrIntern(a,"800-555-9999"); a->phoneLabel[0]=mainLabel; a->displayPhone=0; }

/* ---------- DateBook end-to-end ---------- */
static void testDateBook(void){
    printf("== DateBook ==\n");
    Appt orig[3]={mkAppt1(),mkAppt2(),mkAppt3()};
    uint8_t bytes[3][PALM_REC_MAX]; PdbRec recs[3];
    for(int i=0;i<3;i++){
        int l=ApptPack(bytes[i],PALM_REC_MAX,&orig[i]);
        CK(l>0,"ApptPack ok");
        recs[i]=(PdbRec){ .attr=0, .uniqueID=(uint32_t)(i+1), .data=bytes[i], .len=l };
    }
    pdb_write("pdb/DatebookDB.pdb","DatebookDB",0x44415441,0x64617465,recs,3);

    ApptBag bag={0}; pdb_read("pdb/DatebookDB.pdb",apptCollect,&bag);
    CK(bag.n==3,"read back 3 datebook records");

    for(int i=0;i<bag.n;i++){
        cmpAppt_full(&orig[i],&bag.a[i],"L1 pdb-lossless");
        /* L2: through iCalendar */
        char ics[4096]; ical_emit(ics,sizeof ics,&bag.a[i],i+1);
        Appt back; int pr=ical_parse(ics,&back);
        CK(pr==0,"ical_parse ok");
        cmpAppt_dav(&orig[i],&back,"L2 ics-roundtrip");
        /* L3: parsed-from-DAV repacked into a fresh PDB and re-read */
        uint8_t b2[PALM_REC_MAX]; int l2=ApptPack(b2,PALM_REC_MAX,&back);
        CK(l2>0,"repack ok");
        PdbRec r2={ .attr=0,.uniqueID=(uint32_t)(i+1),.data=b2,.len=l2 };
        pdb_write("pdb/_rt_date.pdb","DatebookDB",0x44415441,0x64617465,&r2,1);
        ApptBag bag2={0}; pdb_read("pdb/_rt_date.pdb",apptCollect,&bag2);
        CK(bag2.n==1,"reread rebuilt datebook");
        cmpAppt_dav(&orig[i],&bag2.a[0],"L3 full-bridge");
    }
}

/* ---------- Address end-to-end ---------- */
static void testAddress(void){
    printf("== Address ==\n");
    Addr orig[2]; mkAddr1(&orig[0]); mkAddr2(&orig[1]);
    uint8_t bytes[2][PALM_REC_MAX]; PdbRec recs[2];
    for(int i=0;i<2;i++){
        int l=AddrPack(bytes[i],PALM_REC_MAX,&orig[i]);
        CK(l>0,"AddrPack ok");
        recs[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=bytes[i],.len=l };
    }
    pdb_write("pdb/AddressDB.pdb","AddressDB",0x44415441,0x61646472,recs,2);

    AddrBag bag={0}; pdb_read("pdb/AddressDB.pdb",addrCollect,&bag);
    CK(bag.n==2,"read back 2 address records");

    for(int i=0;i<bag.n;i++){
        cmpAddr(&orig[i],&bag.a[i],"L1 pdb-lossless",1);
        char vcf[2048]; vcard_emit(vcf,sizeof vcf,&bag.a[i],(uint32_t)(i+1));
        Addr back; int pr=vcard_parse(vcf,&back);
        CK(pr==0,"vcard_parse ok");
        cmpAddr(&orig[i],&back,"L2 vcf-roundtrip",0); /* displayPhone not carried by vCard */
        uint8_t b2[PALM_REC_MAX]; int l2=AddrPack(b2,PALM_REC_MAX,&back);
        CK(l2>0,"repack ok");
        PdbRec r2={ .attr=0,.uniqueID=(uint32_t)(i+1),.data=b2,.len=l2 };
        pdb_write("pdb/_rt_addr.pdb","AddressDB",0x44415441,0x61646472,&r2,1);
        AddrBag bag2={0}; pdb_read("pdb/_rt_addr.pdb",addrCollect,&bag2);
        CK(bag2.n==1,"reread rebuilt address");
        cmpAddr(&orig[i],&bag2.a[0],"L3 full-bridge",0);
    }
}

/* ---------- charset: prove real transcoding, not pass-through ---------- */
static void testCharset(void){
    printf("== Charset (CP1252 <-> UTF-8) ==\n");
    Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.year=2026;a.month=6;a.day=1; a.sH=9;a.eH=10;
    strcpy(a.description,"Caf\xE9");           /* e-acute, one CP1252 byte */
    char ics[2048]; ical_emit(ics,sizeof ics,&a,1);
    CK(strstr(ics,"Caf\xC3\xA9")!=NULL,"SUMMARY emitted as UTF-8 (0xC3 0xA9)");
    CK(strstr(ics,"Caf\xE9")==NULL,"raw CP1252 byte not leaked into ICS");
    Appt b; ical_parse(ics,&b);
    CK(!strcmp(b.description,"Caf\xE9"),"parsed back to CP1252 byte");

    Addr c; memset(&c,0,sizeof c); c.fields[F_company]=AddrIntern(&c,"\xC9""cole");  /* E-acute */
    char vcf[1024]; vcard_emit(vcf,sizeof vcf,&c,1);
    CK(strstr(vcf,"\xC3\x89""cole")!=NULL,"ORG emitted as UTF-8");
    Addr d; vcard_parse(vcf,&d);
    CK(d.fields[F_company]&&!strcmp(d.fields[F_company],"\xC9""cole"),"vCard company back to CP1252");
}

/* ---------- timezone: literal round-trip + UTC conversion w/ DST ---------- */
static int parseICS(const char*dtstart){
    char ics[512];
    snprintf(ics,sizeof ics,"BEGIN:VEVENT\r\nUID:x\r\n%s\r\nSUMMARY:z\r\nEND:VEVENT\r\n",dtstart);
    Appt a; if(ical_parse(ics,&a)) return -1; return a.sH;
}
static void testTZ(void){
    printf("== Timezone (America/New_York) ==\n");
    ical_set_tz("America/New_York");
    /* our own timed event: wall-clock preserved literally + TZID annotated */
    Appt a=mkAppt1();  /* 2026-07-02 14:00, summer */
    char ics[2048]; ical_emit(ics,sizeof ics,&a,1);
    CK(strstr(ics,"TZID=America/New_York")!=NULL,"DTSTART carries TZID");
    CK(strstr(ics,"T140000")!=NULL,"wall-clock preserved literally");
    Appt r; ical_parse(ics,&r);
    CK(r.sH==14&&r.sM==0,"own event parses back to 14:00 (no drift)");
    /* foreign UTC inputs convert to device-local, DST-aware */
    CK(parseICS("DTSTART:20260702T180000Z")==14,"summer 18:00Z -> 14:00 EDT (-4)");
    CK(parseICS("DTSTART:20260115T140000Z")==9, "winter 14:00Z -> 09:00 EST (-5)");
    /* VTIMEZONE emission */
    char vt[1200]; int vl=ical_vtimezone(vt,sizeof vt);
    CK(vl>0&&strstr(vt,"BEGIN:VTIMEZONE")&&strstr(vt,"America/New_York")&&strstr(vt,"BEGIN:DAYLIGHT"),
       "VTIMEZONE block emitted with DST rule");
    ical_set_tz(NULL);   /* back to floating for any later tests */
}

/* ---------- ToDo / VTODO both directions ---------- */
static int todoCollect(const PdbRec*r,int i,void*ctx){ (void)i; struct{Todo t[4];int n;}*b=ctx;
    if(b->n<4 && ToDoUnpack(r->data,r->len,&b->t[b->n])==0) b->n++;
    return 0; }
static void testTodo(void){
    printf("== ToDo ==\n");
    Todo orig[2]; memset(orig,0,sizeof orig);
    orig[0].hasDue=1; orig[0].dueY=2026;orig[0].dueM=8;orig[0].dueD=20; orig[0].priority=1; orig[0].completed=0;
    strcpy(orig[0].description,"File taxes"); strcpy(orig[0].note,"before deadline");
    orig[1].hasDue=0; orig[1].priority=4; orig[1].completed=1; strcpy(orig[1].description,"Call plumber");
    uint8_t bytes[2][PALM_REC_MAX]; PdbRec recs[2];
    for(int i=0;i<2;i++){ int l=ToDoPack(bytes[i],PALM_REC_MAX,&orig[i]); CK(l>0,"ToDoPack ok");
        recs[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=bytes[i],.len=l }; }
    pdb_write("pdb/ToDoDB.pdb","ToDoDB",0x44415441,0x746F646F,recs,2);
    struct{Todo t[4];int n;}bag={.n=0}; pdb_read("pdb/ToDoDB.pdb",todoCollect,&bag);
    CK(bag.n==2,"read back 2 todos");
    for(int i=0;i<bag.n;i++){
        CK(orig[i].hasDue==bag.t[i].hasDue,"hasDue(pdb)");
        if(orig[i].hasDue) CK(orig[i].dueY==bag.t[i].dueY&&orig[i].dueM==bag.t[i].dueM&&orig[i].dueD==bag.t[i].dueD,"due(pdb)");
        CK(orig[i].priority==bag.t[i].priority,"priority(pdb)");
        CK(orig[i].completed==bag.t[i].completed,"completed(pdb)");
        CKSTR(orig[i].description,bag.t[i].description,"desc(pdb)");
        /* through VTODO */
        char vt[2048]; vtodo_emit(vt,sizeof vt,&bag.t[i],i+1);
        Todo back; CK(vtodo_parse(vt,&back)==0,"vtodo_parse ok");
        CK(orig[i].hasDue==back.hasDue,"hasDue(vtodo)");
        if(orig[i].hasDue) CK(orig[i].dueY==back.dueY&&orig[i].dueM==back.dueM&&orig[i].dueD==back.dueD,"due(vtodo)");
        CK(orig[i].priority==back.priority,"priority(vtodo)");
        CK(orig[i].completed==back.completed,"completed(vtodo)");
        CKSTR(orig[i].description,back.description,"desc(vtodo)");
        CKSTR(orig[i].note,back.note,"note(vtodo)");
    }
}

/* ---------- AppInfo category table round-trip ---------- */
static void testAppInfo(void){
    printf("== AppInfo categories ==\n");
    CatTable t; memset(&t,0,sizeof t);
    strcpy(t.name[0],"Unfiled"); strcpy(t.name[1],"Business"); strcpy(t.name[2],"Personal");
    uint8_t ai[APPINFO_SIZE]; int al=appinfo_build(ai,sizeof ai,&t);
    CK(al==APPINFO_SIZE,"appinfo_build size");
    /* write a PDB carrying the AppInfo + a categorized record, read both back */
    Appt a=mkAppt2(); uint8_t rb[PALM_REC_MAX]; int rl=ApptPack(rb,sizeof rb,&a);
    PdbRec r={ .attr=2/*category 2*/,.uniqueID=1,.data=rb,.len=rl };
    pdb_write_ai("pdb/CatDB.pdb","DatebookDB",0x44415441,0x64617465,ai,al,&r,1);
    uint8_t ai2[512]; int al2=pdb_read_appinfo("pdb/CatDB.pdb",ai2,sizeof ai2);
    CK(al2>=APPINFO_SIZE,"appinfo read back");
    CatTable t2; CK(appinfo_parse(ai2,al2,&t2)==0,"appinfo_parse ok");
    CKSTR(t.name[1],t2.name[1],"category 1 label survives");
    CKSTR(t.name[2],t2.name[2],"category 2 label survives");
    /* the record keeps its category nibble */
    static int gotcat; gotcat=-1;
    pdb_read("pdb/CatDB.pdb", catNibble, &gotcat);
    CK(gotcat==2,"record category nibble preserved");
}

int main(void){
    testDateBook();
    testAddress();
    testTodo();
    testAppInfo();
    testCharset();
    testTZ();
    printf("\n%d checks, %d failures\n", checks, fails);
    return fails?1:0;
}
