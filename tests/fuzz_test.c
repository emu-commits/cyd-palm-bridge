/* fuzz_test.c -- negative / malformed-input hardening for every parser the
 * device runs on untrusted data (iCloud objects, PDBs on SD). Built with
 * AddressSanitizer + UBSan (see the Makefile `fuzz_test` target): the pass
 * criterion is simply "no sanitizer report and no crash" across a large sweep
 * of garbage, truncations, and crafted-adversarial inputs.
 *
 * Covered: ical_parse / vtodo_parse / vcard_parse (server text), ApptUnpack /
 * ToDoUnpack / AddrUnpack (packed Palm bytes), appinfo_parse (category table),
 * and the PDB container (pdb_read / pdb_read_one / pdb_read_appinfo).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/appinfo.h"

#define TMP "state/fuzz.pdb"

/* deterministic xorshift so a failure is reproducible */
static uint32_t R=0x1234567u;
static uint32_t xr(void){ R^=R<<13; R^=R>>17; R^=R<<5; return R; }
static void fill(uint8_t*b,int n){ for(int i=0;i<n;i++) b[i]=(uint8_t)xr(); }

/* run every text parser on a null-terminated string */
static void text_parsers(const char*s){
    Appt a; ical_parse(s,&a);
    Todo t; vtodo_parse(s,&t);
    Addr d; vcard_parse(s,&d);
}
/* run every unpacker on raw bytes */
static void unpackers(const uint8_t*b,int n){
    Appt a; ApptUnpack(b,n,&a);
    Todo t; ToDoUnpack(b,n,&t);
    Addr d; AddrUnpack(b,n,&d);
    CatTable ct; appinfo_parse(b,n,&ct);
}
static int recCb(const PdbRec*r,int i,void*c){ (void)i;(void)c; unpackers(r->data,r->len); return 0; }
static void pdb_paths(const uint8_t*b,int n){
    FILE*f=fopen(TMP,"wb"); if(!f) return; fwrite(b,1,n,f); fclose(f);
    pdb_read(TMP,recCb,NULL);
    uint8_t buf[PALM_REC_MAX]; uint8_t at; uint32_t uid;
    for(int k=-1;k<6;k++) pdb_read_one(TMP,k,buf,sizeof buf,&at,&uid);
    uint8_t ai[512]; pdb_read_appinfo(TMP,ai,sizeof ai);
}

/* build valid objects so we can truncate them at every length */
static int mk_vevent(char*o,int cap){
    Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=9;a.eH=10; a.year=2026;a.month=6;a.day=1;
    a.hasAlarm=1; a.alarmAdv=10; a.hasRepeat=1; a.repeatType=repeatWeekly; a.repeatFreq=1;
    a.nExcept=1; a.excpt[0].y=2026;a.excpt[0].m=6;a.excpt[0].d=8;
    snprintf(a.description,sizeof a.description,"Fuzz me");
    snprintf(a.note,sizeof a.note,"line1\nline2");
    char v[4096]; ical_emit(v,sizeof v,&a,1);
    return snprintf(o,cap,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\n%sEND:VCALENDAR\r\n",v);
}
static int mk_vtodo(char*o,int cap){
    Todo t; memset(&t,0,sizeof t); t.priority=2; t.hasDue=1; t.dueY=2026;t.dueM=6;t.dueD=1;
    snprintf(t.description,sizeof t.description,"Todo fuzz");
    char v[2048]; vtodo_emit(v,sizeof v,&t,2);
    return snprintf(o,cap,"BEGIN:VCALENDAR\r\nVERSION:2.0\r\n%sEND:VCALENDAR\r\n",v);
}
static int mk_vcard(char*o,int cap){
    Addr d; memset(&d,0,sizeof d);
    d.fields[F_name]=AddrIntern(&d,"Doe"); d.fields[F_firstName]=AddrIntern(&d,"Jane");
    d.fields[F_company]=AddrIntern(&d,"Acme"); d.fields[F_phone1]=AddrIntern(&d,"555");
    return vcard_emit(o,cap,&d,3);
}
static int mk_appt_bytes(uint8_t*o,int cap){
    Appt a; memset(&a,0,sizeof a); a.hasTime=1; a.sH=9;a.eH=10; a.year=2026;a.month=6;a.day=1;
    snprintf(a.description,sizeof a.description,"x"); return ApptPack(o,cap,&a);
}

/* truncate a valid buffer at every length and re-parse */
static void truncate_text(const char*full){
    int L=(int)strlen(full);
    char*buf=malloc(L+1);
    for(int n=0;n<=L;n++){ memcpy(buf,full,n); buf[n]=0; text_parsers(buf); }
    free(buf);
}
static void truncate_bytes(const uint8_t*full,int L){
    for(int n=0;n<=L;n++) unpackers(full,n);
}

int main(void){
    printf("== parser hardening (ASan/UBSan) ==\n");

    /* 1. truncate valid objects at every byte boundary */
    char ev[6000]; int el=mk_vevent(ev,sizeof ev); truncate_text(ev);
    char td[4000]; mk_vtodo(td,sizeof td); truncate_text(td);
    char vc[4000]; mk_vcard(vc,sizeof vc); truncate_text(vc);
    printf("  truncation sweep ok (vevent=%d bytes)\n",el);

    uint8_t ab[PALM_REC_MAX]; int al=mk_appt_bytes(ab,sizeof ab); truncate_bytes(ab,al);
    printf("  packed-record truncation ok (%d bytes)\n",al);

    /* 2. adversarial fixed strings */
    const char*evil[]={
        "", "BEGIN:VCALENDAR", "BEGIN:VEVENT\r\nDTSTART:", "DTSTART:garbage",
        "BEGIN:VEVENT\r\nRRULE:FREQ=;COUNT=\r\nEND:VEVENT",
        "BEGIN:VCARD\r\nFN:\r\nTEL;TYPE=:\r\nEND:VCARD",
        "EXDATE:99999999T999999\r\n", "DTSTART;TZID=:\r\n",
        "BEGIN:VTODO\r\nPRIORITY:999\r\nDUE:0\r\nEND:VTODO",
        "\r\n\r\n\r\n", "::::::", "BEGIN:VEVENT\nSUMMARY:\xff\xfe\xfd\n" };
    for(int i=0;i<(int)(sizeof evil/sizeof evil[0]);i++) text_parsers(evil[i]);
    printf("  adversarial strings ok\n");

    /* 3. crafted PDB headers: huge nrec, bogus offsets, tiny files */
    { uint8_t h[78]; memset(h,0,sizeof h); h[0x4C]=0xFF; h[0x4D]=0xFF; /* nrec=65535 */
      pdb_paths(h,sizeof h); }
    { uint8_t h[78]; memset(h,0,sizeof h); h[0x4D]=0x08; /* nrec=8, no index/data */
      pdb_paths(h,sizeof h); }
    { uint8_t tiny[10]={0}; pdb_paths(tiny,sizeof tiny); }          /* shorter than header */
    { uint8_t h[100]; memset(h,0,sizeof h); h[0x4D]=2;              /* 2 recs, garbage index */
      for(int i=78;i<100;i++) h[i]=(uint8_t)xr();
      pdb_paths(h,sizeof h); }
    printf("  crafted PDB headers ok\n");

    /* 4. random fuzz: many small random buffers through every path */
    int iters=120000;
    uint8_t rb[4096]; char rs[4097];
    for(int it=0; it<iters; it++){
        int n = (int)(xr()% (sizeof rb));
        fill(rb,n);
        unpackers(rb,n);
        memcpy(rs,rb,n); rs[n]=0; text_parsers(rs);
        if((it & 0x3FF)==0) pdb_paths(rb,n);      /* PDB path is slower; sample it */
    }
    printf("  random fuzz: %d iterations, no crash / no sanitizer report\n",iters);

    printf("\nALL PASS (0 failures)\n");
    return 0;
}
