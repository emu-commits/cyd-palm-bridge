/* find_test.c -- exercise the global Find across all four app kinds. No server. */
#include <stdio.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/find.h"

static int fails=0;
static void CK(int c,const char*m){ if(!c){ fails++; printf("  FAIL: %s\n",m);} else printf("  ok: %s\n",m); }

/* collect hits */
typedef struct { int n; char last[96]; uint32_t uids[16]; } Hits;
static void hitCb(const FindHit*h,void*ctx){ Hits*H=ctx; if(H->n<16) H->uids[H->n]=h->uid;
    snprintf(H->last,sizeof H->last,"%s",h->snippet); H->n++; }

static void buildCal(const char*p){
    uint8_t arena[8*PALM_REC_MAX]; PdbRec r[8]; int used=0;
    struct { const char*d; const char*note; } ev[]={
        {"Dentist appointment","bring insurance card"},
        {"Team meeting","discuss Q3 budget"},
        {"Lunch with Sam",""} };
    for(int i=0;i<3;i++){ Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=9; a.eH=10;
        a.year=2026; a.month=7; a.day=1+i;
        snprintf(a.description,sizeof a.description,"%s",ev[i].d);
        snprintf(a.note,sizeof a.note,"%s",ev[i].note);
        uint8_t*dst=arena+used; int l=ApptPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l; }
    pdb_write(p,"DatebookDB",0x44415441,0x64617465,r,3);
}
static void buildTodo(const char*p){
    uint8_t arena[8*PALM_REC_MAX]; PdbRec r[8]; int used=0;
    const char*d[]={"Buy milk","Call dentist","File taxes"};
    for(int i=0;i<3;i++){ Todo t; memset(&t,0,sizeof t); t.priority=1;
        snprintf(t.description,sizeof t.description,"%s",d[i]);
        uint8_t*dst=arena+used; int l=ToDoPack(dst,PALM_REC_MAX,&t);
        r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l; }
    pdb_write(p,"ToDoDB",0x44415441,0x746F646F,r,3);
}
static void buildAddr(const char*p){
    uint8_t arena[8*PALM_REC_MAX]; PdbRec r[8]; int used=0;
    struct { const char*last; const char*co; const char*ph; } c[]={
        {"Robinson","Acme Corp","555-1212"},
        {"Nakamura","Globex","555-3434"} };
    for(int i=0;i<2;i++){ Addr a; memset(&a,0,sizeof a);
        a.fields[F_name]=AddrIntern(&a,c[i].last);
        a.fields[F_company]=AddrIntern(&a,c[i].co);
        a.fields[F_phone1]=AddrIntern(&a,c[i].ph); a.phoneLabel[0]=workLabel;
        uint8_t*dst=arena+used; int l=AddrPack(dst,PALM_REC_MAX,&a);
        r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),.data=dst,.len=l }; used+=l; }
    pdb_write(p,"AddressDB",0x44415441,0x61646472,r,2);
}
static void buildMemo(const char*p){
    PdbRec r[8]; int n=0;
    static const char*txt[]={
        "Shopping list\nmilk, eggs, bread",
        "Ideas for the garden project",
        "WiFi password is on the fridge" };
    for(int i=0;i<3;i++){ r[n]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),
        .data=(const uint8_t*)txt[i],.len=(int)strlen(txt[i]) }; n++; }
    pdb_write(p,"MemoDB",0x44415441,0x6D656D6F,r,3);
}

static int run(const char*path,int app,const char*q){
    Hits H={0}; int rc=find_in_pdb(path,app,q,hitCb,&H);
    printf("  find '%s' in %s -> %d hits%s%s\n",q,path,rc,H.n?" e.g. \"":"",H.n?H.last:"");
    if(H.n) printf("\"\n");
    return rc;
}

int main(void){
    const char*CAL="pdb/f_cal.pdb",*TD="pdb/f_todo.pdb",*AD="pdb/f_addr.pdb",*MM="pdb/f_memo.pdb";
    buildCal(CAL); buildTodo(TD); buildAddr(AD); buildMemo(MM);

    printf("== Find across apps ==\n");
    /* case-insensitive substring, spanning description + note + all addr fields */
    CK(run(CAL,FIND_CAL,"dentist")==1,"cal: 'dentist' matches description");
    CK(run(CAL,FIND_CAL,"BUDGET")==1,"cal: case-insensitive note match");
    CK(run(CAL,FIND_CAL,"a")>=2,"cal: common letter matches several");
    CK(run(TD,FIND_TODO,"dentist")==1,"todo: matches 'Call dentist'");
    CK(run(AD,FIND_ADDR,"acme")==1,"addr: matches company field");
    CK(run(AD,FIND_ADDR,"555")==2,"addr: phone substring matches both");
    CK(run(AD,FIND_ADDR,"robinson")==1,"addr: last-name match, case-insensitive");
    CK(run(MM,FIND_MEMO,"wifi")==1,"memo: plain-text body match");
    CK(run(MM,FIND_MEMO,"eggs")==1,"memo: match past a newline");

    /* negative + edge cases */
    CK(run(CAL,FIND_CAL,"zzz")==0,"no match returns 0");
    CK(run(CAL,FIND_CAL,"")==0,"empty query matches nothing");
    CK(find_in_pdb("pdb/does_not_exist.pdb",FIND_CAL,"x",hitCb,0)==-1,"missing file -> -1");

    /* snippet sanity: 'dentist' hit carries readable context */
    Hits H={0}; find_in_pdb(CAL,FIND_CAL,"dentist",hitCb,&H);
    CK(H.n==1 && strstr(H.last,"Dentist")!=NULL,"snippet contains the matched text");

    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
