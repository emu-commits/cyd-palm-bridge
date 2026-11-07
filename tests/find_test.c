/* find_test.c -- Find over a Memos database. No server. */
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

static void buildMemo(const char*p){
    PdbRec r[8]; int n=0;
    static const char*txt[]={
        "Shopping list\nmilk, eggs, bread",
        "Ideas for the garden project",
        "WiFi password is on the fridge",
        "How to use a lathe",
        "Dentist appointment Thursday, bring the card" };
    for(int i=0;i<5;i++){ r[n]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),
        .data=(const uint8_t*)txt[i],.len=(int)strlen(txt[i]) }; n++; }
    /* one an older build marked deleted (awaiting a sync that no longer exists) */
    static const char *gone = "Dentist reminder that was deleted";
    r[n]=(PdbRec){ .attr=REC_ATTR_DELETE,.uniqueID=9,.data=(const uint8_t*)gone,.len=(int)strlen(gone) }; n++;
    pdb_write(p,"MemoDB",0x44415441,0x6D656D6F,r,n);
}

static int run(const char*path,const char*q){
    Hits H={0}; int rc=find_in_pdb(path,FIND_MEMO,q,hitCb,&H);
    printf("  find '%s' in %s -> %d hits%s%s\n",q,path,rc,H.n?" e.g. \"":"",H.n?H.last:"");
    if(H.n) printf("\"\n");
    return rc;
}

int main(void){
    const char*MM="pdb/f_memo.pdb";
    buildMemo(MM);

    printf("== Find in Memos ==\n");
    CK(run(MM,"wifi")==1,"plain-text body match");
    CK(run(MM,"eggs")==1,"match past a newline");
    CK(run(MM,"DENTIST")==1,"case-insensitive, and a deleted record is skipped");
    CK(run(MM,"a")==2,"one letter finds only words that START with it (a lathe, appointment)");

    /* Palm's own example: "the" finds "Then", not "Lathe" */
    CK(run(MM,"the")==3,"'the' finds 'the garden', 'the fridge' and 'the card', not 'lathe'");
    CK(run(MM,"lathe")==1,"...though 'lathe' itself is found");
    CK(run(MM,"arden")==0,"the middle of a word is not a match");
    CK(run(MM,"ilk")==0,"'ilk' is inside 'milk', so no match");

    /* negative + edge cases */
    CK(run(MM,"zzz")==0,"no match returns 0");
    CK(run(MM,"")==0,"empty query matches nothing");
    CK(find_in_pdb("pdb/does_not_exist.pdb",FIND_MEMO,"x",hitCb,0)==-1,"missing file -> -1");

    /* snippet sanity: the hit carries readable context */
    Hits H={0}; find_in_pdb(MM,FIND_MEMO,"dentist",hitCb,&H);
    CK(H.n==1 && strstr(H.last,"Dentist")!=NULL,"snippet contains the matched text");

    printf("\n%s (%d failures)\n", fails?"FAILURES":"ALL PASS", fails);
    return fails?1:0;
}
