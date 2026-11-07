/* fuzz_test.c -- negative / malformed-input hardening for the parsers the
 * device runs on files off the SD card. Built with AddressSanitizer + UBSan
 * (see the Makefile `fuzz_test` target): the pass criterion is simply "no
 * sanitizer report and no crash" across a large sweep of garbage, truncations
 * and crafted-adversarial inputs.
 *
 * Covered: appinfo_parse (the category table) and the PDB container
 * (pdb_read / pdb_read_one / pdb_read_appinfo), the parts of the Memos
 * database's path that read bytes. (The iCalendar/vCard parsers and the
 * Date Book/To Do/Address unpackers it also covered were archived on
 * 2026-11-07: git tag archive/pim-caldav.)
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

/* the category table parser on raw bytes */
static void unpackers(const uint8_t*b,int n){
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

int main(void){
    printf("== parser hardening (ASan/UBSan) ==\n");

    /* 1. a valid category table, truncated at every byte */
    { CatTable t; memset(&t,0,sizeof t);
      strcpy(t.name[0],"Unfiled"); strcpy(t.name[1],"Business"); strcpy(t.name[2],"Personal");
      uint8_t ai[APPINFO_SIZE]; int al = appinfo_build(ai, sizeof ai, &t);
      for(int n=0;n<=al;n++) unpackers(ai,n);
      printf("  category table truncation ok (%d bytes)\n",al); }

    /* 2. a valid memo database, truncated at every byte */
    { PdbRec r[3]; static const char *txt[] = { "one", "two\nlines", "three" };
      for(int i=0;i<3;i++) r[i]=(PdbRec){ .attr=0,.uniqueID=(uint32_t)(i+1),
          .data=(const uint8_t*)txt[i],.len=(int)strlen(txt[i])+1 };
      pdb_write(TMP,"MemoDB",0x44415441,0x6D656D6F,r,3);
      FILE *f=fopen(TMP,"rb"); uint8_t full[1024]; int L=f?(int)fread(full,1,sizeof full,f):0; if(f) fclose(f);
      for(int n=0;n<=L;n++) pdb_paths(full,n);
      printf("  memo database truncation ok (%d bytes)\n",L); }

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
    uint8_t rb[4096];
    for(int it=0; it<iters; it++){
        int n = (int)(xr()% (sizeof rb));
        fill(rb,n);
        unpackers(rb,n);
        if((it & 0x3FF)==0) pdb_paths(rb,n);      /* PDB path is slower; sample it */
    }
    printf("  random fuzz: %d iterations, no crash / no sanitizer report\n",iters);

    printf("\nALL PASS (0 failures)\n");
    return 0;
}
