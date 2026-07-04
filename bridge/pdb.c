/* pdb.c -- PalmOS .pdb container: streaming reader + writer.
 *
 * Clean-room from the public PDB format (78-byte header, 8-byte record index
 * entries), NOT lifted from PumpkinOS -- keeps the firmware license-free.
 *
 * Reader holds exactly ONE record in RAM at a time (PALM_REC_MAX). Writer
 * takes an array of already-packed records and lays out header+index+data.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "palm.h"

#define PDB_HDR   78
#define PDB_ENTRY 8

int pdb_read(const char *path, pdb_rec_cb cb, void *ctx){
    FILE *f = fopen(path, "rb");
    if(!f){ return -1; }
    uint8_t H[PDB_HDR];
    if(fread(H,1,PDB_HDR,f)!=PDB_HDR){ fclose(f); return -1; }
    int nrec = be16(H + 0x4C);
    if(nrec < 0){ fclose(f); return -1; }

    /* read the whole record index first (8 bytes each is tiny) so we can
     * compute each record's length from the next offset / EOF.             */
    uint32_t *off = malloc(sizeof(uint32_t)*(nrec+1));
    uint8_t  *att = malloc((size_t)nrec+1);
    uint32_t *uid = malloc(sizeof(uint32_t)*(nrec+1));
    if(!off||!att||!uid){ free(off);free(att);free(uid); fclose(f); return -1; }
    for(int i=0;i<nrec;i++){
        uint8_t e[PDB_ENTRY];
        if(fread(e,1,PDB_ENTRY,f)!=PDB_ENTRY){ free(off);free(att);free(uid); fclose(f); return -1; }
        off[i]=be32(e);
        att[i]=e[4];
        uid[i]=((uint32_t)e[5]<<16)|((uint32_t)e[6]<<8)|e[7];
    }
    fseek(f,0,SEEK_END);
    off[nrec]=(uint32_t)ftell(f);

    uint8_t buf[PALM_REC_MAX];
    int count=0;
    for(int i=0;i<nrec;i++){
        long len = (long)off[i+1]-(long)off[i];
        if(len<0) len=0;
        if(len>PALM_REC_MAX) len=PALM_REC_MAX;
        fseek(f,off[i],SEEK_SET);
        if(len && fread(buf,1,(size_t)len,f)!=(size_t)len){ break; }
        PdbRec rec = { .attr=att[i], .uniqueID=uid[i], .data=buf, .len=(int)len };
        count++;
        if(cb && cb(&rec,i,ctx)) break;
    }
    free(off);free(att);free(uid);
    fclose(f);
    return count;
}

int pdb_write_ai(const char *path, const char *name,
                 uint32_t type, uint32_t creator,
                 const uint8_t *appinfo, int ailen,
                 const PdbRec *recs, int nrecs){
    FILE *f = fopen(path,"wb");
    if(!f) return -1;
    if(ailen<0) ailen=0;

    /* record index + 2-byte pad, then AppInfo, then records */
    uint32_t aiOff  = PDB_HDR + (uint32_t)PDB_ENTRY*nrecs + 2;
    uint32_t dataStart = aiOff + (uint32_t)ailen;

    uint8_t H[PDB_HDR];
    memset(H,0,sizeof H);
    strncpy((char*)H, name, 31);
    uint32_t now = 0; /* deterministic for round-trip tests; real sync sets it */
    put32(H+0x24, now); put32(H+0x28, now); put32(H+0x2C, 0);
    put32(H+0x30, 1);
    put32(H+0x34, ailen ? aiOff : 0);   /* appInfoID */
    put32(H+0x38, 0);                   /* sortInfoID */
    put32(H+0x3C, type);
    put32(H+0x40, creator);
    put32(H+0x44, 0);
    put32(H+0x48, 0);
    put16(H+0x4C, (uint16_t)nrecs);
    fwrite(H,1,PDB_HDR,f);

    uint32_t off = dataStart;
    for(int i=0;i<nrecs;i++){
        uint8_t e[PDB_ENTRY];
        put32(e, off);
        e[4]=recs[i].attr;
        e[5]=(uint8_t)(recs[i].uniqueID>>16);
        e[6]=(uint8_t)(recs[i].uniqueID>>8);
        e[7]=(uint8_t)(recs[i].uniqueID);
        fwrite(e,1,PDB_ENTRY,f);
        off += (uint32_t)recs[i].len;
    }
    uint8_t pad[2]={0,0};
    fwrite(pad,1,2,f);
    if(ailen) fwrite(appinfo,1,(size_t)ailen,f);
    for(int i=0;i<nrecs;i++)
        if(recs[i].len) fwrite(recs[i].data,1,(size_t)recs[i].len,f);
    fclose(f);
    return nrecs;
}

int pdb_write(const char *path, const char *name,
              uint32_t type, uint32_t creator,
              const PdbRec *recs, int nrecs){
    return pdb_write_ai(path,name,type,creator,NULL,0,recs,nrecs);
}

int pdb_read_appinfo(const char *path, uint8_t *buf, int cap){
    FILE *f = fopen(path,"rb"); if(!f) return -1;
    uint8_t H[PDB_HDR];
    if(fread(H,1,PDB_HDR,f)!=PDB_HDR){ fclose(f); return -1; }
    uint32_t aiOff = be32(H+0x34);
    if(aiOff==0){ fclose(f); return 0; }
    uint32_t sortOff = be32(H+0x38);
    int nrec = be16(H+0x4C);
    /* AppInfo ends at sortInfo, or first record, or EOF */
    uint32_t end = 0;
    if(nrec>0){ uint8_t e[PDB_ENTRY]; fseek(f,PDB_HDR,SEEK_SET);
        if(fread(e,1,PDB_ENTRY,f)==PDB_ENTRY) end=be32(e); }
    if(sortOff && (!end || sortOff<end)) end=sortOff;
    if(!end){ fseek(f,0,SEEK_END); end=(uint32_t)ftell(f); }
    long len = (long)end - (long)aiOff;
    if(len<0) len=0;
    if(len>cap) len=cap;
    fseek(f,aiOff,SEEK_SET);
    int got = (len>0) ? (int)fread(buf,1,(size_t)len,f) : 0;
    fclose(f);
    return got;
}
