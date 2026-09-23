/* pdb.c -- PalmOS .pdb container: streaming reader + writer.
 *
 * Clean-room from the public PDB format (78-byte header, 8-byte record index
 * entries), NOT lifted from PumpkinOS -- keeps the firmware license-free.
 *
 * Reader holds exactly ONE record in RAM at a time (PALM_REC_MAX). Two writer
 * shapes: pdb_write_ai() lays out an in-RAM array of records in one call; the
 * PdbW streaming writer (pdbw_*) spills record bytes to a temp file as they are
 * decided and keeps only a tiny per-record index, so the sync engine never has
 * to hold the whole output database in RAM (the no-PSRAM device constraint).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "palm.h"
#include "safefile.h"   /* write .tmp, swap in: a crash never leaves half a PDB */

#define PDB_HDR   78
#define PDB_ENTRY 8
/* sanity cap on the header's record count: a corrupt/hostile PDB claiming e.g.
 * 65535 records must not trigger a ~0.6 MB index allocation on a device with no
 * PSRAM. Real Palm PIM databases are a few hundred to a few thousand records. */
#define PDB_MAX_RECS 20000

/* ---- failing loudly ------------------------------------------------------
 * pdb_read returns -1 for every kind of failure, and NEARLY EVERY CALLER IN
 * THIS TREE IGNORES THE RETURN VALUE -- they hand in a callback and read what
 * lands. That turns "the card is unreadable" into "the database is empty", and
 * an empty local database is precisely what the sync engine pushes to the
 * server AS DELETIONS. The mass-delete guard catches the worst of it, but a
 * guard that fires is the second line of defence; this is the first.
 *
 * So every exit from the read path now says which one it was, once, on stderr
 * (the UART on device). Nothing else changes: the return value is the same, and
 * a caller that already checks it keeps working. */
static int pdb_fail(FILE *f, const char *path, const char *why){
    if(f) fclose(f);
    fprintf(stderr, "[pdb] %s UNREADABLE: %s. Treat this as \"cannot tell\", "
                    "NOT as \"no records\".\n", path, why);
    return -1;
}

int pdb_read(const char *path, pdb_rec_cb cb, void *ctx){
    sf_recover(path);          /* finish a swap a crash interrupted (safefile.h) */
    FILE *f = fopen(path, "rb");
    /* A missing file is the one quiet case, and deliberately: first boot has no
     * databases yet and data_seed_if_empty() asks about each of them in turn. */
    if(!f){ return -1; }
    uint8_t H[PDB_HDR];
    if(fread(H,1,PDB_HDR,f)!=PDB_HDR) return pdb_fail(f,path,"shorter than a 78-byte PDB header");
    int nrec = be16(H + 0x4C);
    if(nrec < 0) return pdb_fail(f,path,"header declares a negative record count");
    if(nrec > PDB_MAX_RECS){
        /* Not a corruption check that can be relaxed: the index alloc below is
         * 9 bytes a record, and this device has no PSRAM. Say the number. */
        fprintf(stderr,"[pdb] %s UNREADABLE: header declares %d records, over the "
                       "%d cap. Treat this as \"cannot tell\", NOT as \"no records\".\n",
                path, nrec, PDB_MAX_RECS);
        fclose(f); return -1;
    }

    /* read the whole record index first (8 bytes each is tiny) so we can
     * compute each record's length from the next offset / EOF.             */
    uint32_t *off = malloc(sizeof(uint32_t)*(nrec+1));
    uint8_t  *att = malloc((size_t)nrec+1);
    uint32_t *uid = malloc(sizeof(uint32_t)*(nrec+1));
    if(!off||!att||!uid){ free(off);free(att);free(uid);
        return pdb_fail(f,path,"out of memory for the record index"); }
    for(int i=0;i<nrec;i++){
        uint8_t e[PDB_ENTRY];
        if(fread(e,1,PDB_ENTRY,f)!=PDB_ENTRY){ free(off);free(att);free(uid);
            return pdb_fail(f,path,"record index is truncated"); }
        off[i]=be32(e);
        att[i]=e[4];
        uid[i]=((uint32_t)e[5]<<16)|((uint32_t)e[6]<<8)|e[7];
    }
    fseek(f,0,SEEK_END);
    off[nrec]=(uint32_t)ftell(f);

    uint8_t buf[PALM_REC_MAX];
    int count=0;
    int shortRead=0;
    for(int i=0;i<nrec;i++){
        long len = (long)off[i+1]-(long)off[i];
        if(len<0) len=0;
        if(len>PALM_REC_MAX) len=PALM_REC_MAX;
        fseek(f,off[i],SEEK_SET);
        if(len && fread(buf,1,(size_t)len,f)!=(size_t)len){ shortRead=1; break; }
        PdbRec rec = { .attr=att[i], .uniqueID=uid[i], .data=buf, .len=(int)len };
        count++;
        if(cb && cb(&rec,i,ctx)) break;
    }
    /* A file that stops mid-record returns a POSITIVE partial count, which no
     * caller can distinguish from a genuinely shorter database -- and the sync
     * engine would read the difference as records the user deleted. It still
     * returns the partial (some callers only display), but it does not do it
     * silently. app_main.c's boot-time "DATABASE IS SHORT" check exists for the
     * same reason and compares this count against the header. */
    if(shortRead)
        fprintf(stderr,"[pdb] %s IS SHORT: header declares %d records, only %d are "
                       "readable. The %d missing are NOT deletions.\n",
                path, nrec, count, nrec-count);
    free(off);free(att);free(uid);
    fclose(f);
    return count;
}

/* random-access read of the record at index `want` (0-based, as handed to the
 * pdb_read callback). Fills buf (up to cap) and, if non-NULL, attr/uid.
 * Returns the record's byte length (>=0) or -1 on error / bad index. Lets the
 * sync engine load a local record's bytes lazily instead of buffering them all
 * in a big arena -- reopening per record is cheap on SD and frees the RAM. */
int pdb_read_one(const char *path, int want, uint8_t *buf, int cap,
                 uint8_t *attr, uint32_t *uid){
    if(want < 0) return -1;
    sf_recover(path);
    FILE *f = fopen(path,"rb");
    if(!f) return -1;
    uint8_t H[PDB_HDR];
    if(fread(H,1,PDB_HDR,f)!=PDB_HDR){ fclose(f); return -1; }
    int nrec = be16(H + 0x4C);
    /* `want >= nrec` is an ordinary end-of-database answer and stays quiet; the
     * cap is a broken file and says so, for the reason pdb_fail() explains. */
    if(nrec > PDB_MAX_RECS)
        return pdb_fail(f,path,"header declares more records than the cap allows");
    if(want >= nrec){ fclose(f); return -1; }

    /* we only need offsets[want] and offsets[want+1] (or EOF). Read the index
     * up to want+1; the entry for `want` also carries attr + uid.            */
    uint32_t o0=0, o1=0; uint8_t a=0; uint32_t u=0;
    for(int i=0;i<=want;i++){
        uint8_t e[PDB_ENTRY];
        if(fread(e,1,PDB_ENTRY,f)!=PDB_ENTRY){ fclose(f); return -1; }
        if(i==want){ o0=be32(e); a=e[4]; u=((uint32_t)e[5]<<16)|((uint32_t)e[6]<<8)|e[7]; }
    }
    if(want+1 < nrec){
        uint8_t e[PDB_ENTRY];
        if(fread(e,1,PDB_ENTRY,f)!=PDB_ENTRY){ fclose(f); return -1; }
        o1=be32(e);
    } else {
        fseek(f,0,SEEK_END); o1=(uint32_t)ftell(f);
    }
    long len=(long)o1-(long)o0; if(len<0) len=0;
    if(len>cap) len=cap;
    fseek(f,o0,SEEK_SET);
    int got = (len>0) ? (int)fread(buf,1,(size_t)len,f) : 0;
    fclose(f);
    if(attr) *attr=a;
    if(uid)  *uid=u;
    return got;
}

/* fill the 78-byte header. Single source of truth for both writers. */
static void pdb_put_header(uint8_t H[PDB_HDR], const char *name,
                           uint32_t type, uint32_t creator,
                           int nrecs, uint32_t aiOff, int ailen){
    memset(H,0,PDB_HDR);
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
}

int pdb_write_ai(const char *path, const char *name,
                 uint32_t type, uint32_t creator,
                 const uint8_t *appinfo, int ailen,
                 const PdbRec *recs, int nrecs){
    /* written beside the live file and swapped in at the end (safefile.h):
     * the database on the card is the old one or the new one, never half */
    SafeFile sf;
    FILE *f = sf_open(&sf, path, "wb");
    if(!f) return -1;
    if(ailen<0) ailen=0;

    /* record index + 2-byte pad, then AppInfo, then records */
    uint32_t aiOff  = PDB_HDR + (uint32_t)PDB_ENTRY*nrecs + 2;
    uint32_t dataStart = aiOff + (uint32_t)ailen;

    uint8_t H[PDB_HDR];
    pdb_put_header(H,name,type,creator,nrecs,aiOff,ailen);
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
    if(sf_commit(&sf, !ferror(f)) != 0) return -1;     /* a full card, a bad write */
    return nrecs;
}

int pdb_write(const char *path, const char *name,
              uint32_t type, uint32_t creator,
              const PdbRec *recs, int nrecs){
    return pdb_write_ai(path,name,type,creator,NULL,0,recs,nrecs);
}

/* ===================== streaming PDB writer (PdbW) ====================== */
/* record bytes go to a temp file as pdbw_rec() is called; only this tiny index
 * (24 bytes/record) stays in RAM. pdbw_commit() sorts by uniqueID and assembles
 * the final PDB, copying record bytes back from the temp file. The index grows
 * on demand, so the output is bounded by disk, not by a fixed RAM arena.      */
typedef struct { uint32_t uid; uint8_t attr; uint32_t len; long tmpoff; } PdbwEnt;
struct PdbW {
    char   tmppath[256];
    FILE  *tmp;
    long   tmpused;
    PdbwEnt *ent; int nent, cap;
};

PdbW *pdbw_begin(const char *tmppath){
    PdbW *w = calloc(1,sizeof *w);
    if(!w) return NULL;
    snprintf(w->tmppath,sizeof w->tmppath,"%s",tmppath);
    w->tmp = fopen(tmppath,"wb+");
    if(!w->tmp){ free(w); return NULL; }
    return w;
}

int pdbw_rec(PdbW *w, uint32_t uid, uint8_t attr, const uint8_t *data, int len){
    if(!w) return -1;
    if(len<0) len=0;
    if(w->nent==w->cap){
        int nc = w->cap ? w->cap*2 : 32;
        PdbwEnt *e = realloc(w->ent, (size_t)nc*sizeof *e);
        if(!e) return -1;
        w->ent=e; w->cap=nc;
    }
    if(len && fwrite(data,1,(size_t)len,w->tmp)!=(size_t)len) return -1;
    w->ent[w->nent] = (PdbwEnt){ uid, attr, (uint32_t)len, w->tmpused };
    w->tmpused += len;
    w->nent++;
    return 0;
}

int pdbw_count(const PdbW *w){ return w ? w->nent : 0; }

void pdbw_abort(PdbW *w){
    if(!w) return;
    if(w->tmp) fclose(w->tmp);
    remove(w->tmppath);
    free(w->ent);
    free(w);
}

static int pdbwCmp(const void *a,const void *b){
    uint32_t ua=((const PdbwEnt*)a)->uid, ub=((const PdbwEnt*)b)->uid;
    return ua<ub ? -1 : ua>ub ? 1 : 0;
}

int pdbw_commit(PdbW *w, const char *path, const char *name,
                uint32_t type, uint32_t creator,
                const uint8_t *appinfo, int ailen){
    if(!w) return -1;
    if(ailen<0) ailen=0;
    int nrecs = w->nent;
    qsort(w->ent, nrecs, sizeof w->ent[0], pdbwCmp);

    SafeFile sf;                                   /* see pdb_write_ai */
    FILE *f = sf_open(&sf, path, "wb");
    if(!f){ pdbw_abort(w); return -1; }

    uint32_t aiOff = PDB_HDR + (uint32_t)PDB_ENTRY*nrecs + 2;
    uint32_t dataStart = aiOff + (uint32_t)ailen;

    uint8_t H[PDB_HDR];
    pdb_put_header(H,name,type,creator,nrecs,aiOff,ailen);
    fwrite(H,1,PDB_HDR,f);

    uint32_t off = dataStart;
    for(int i=0;i<nrecs;i++){
        uint8_t e[PDB_ENTRY];
        put32(e, off);
        e[4]=w->ent[i].attr;
        e[5]=(uint8_t)(w->ent[i].uid>>16);
        e[6]=(uint8_t)(w->ent[i].uid>>8);
        e[7]=(uint8_t)(w->ent[i].uid);
        fwrite(e,1,PDB_ENTRY,f);
        off += w->ent[i].len;
    }
    uint8_t pad[2]={0,0};
    fwrite(pad,1,2,f);
    if(ailen) fwrite(appinfo,1,(size_t)ailen,f);

    /* copy record bytes from the temp file in sorted order */
    uint8_t cbuf[1024];
    fflush(w->tmp);
    for(int i=0;i<nrecs;i++){
        long remaining = w->ent[i].len;
        /* a failure here used to fclose() the half-built PDB and leave it as
         * THE database; now the live file is never touched unless it all worked */
        if(fseek(w->tmp, w->ent[i].tmpoff, SEEK_SET)!=0){ sf_abort(&sf); pdbw_abort(w); return -1; }
        while(remaining>0){
            size_t chunk = remaining < (long)sizeof cbuf ? (size_t)remaining : sizeof cbuf;
            if(fread(cbuf,1,chunk,w->tmp)!=chunk){ sf_abort(&sf); pdbw_abort(w); return -1; }
            fwrite(cbuf,1,chunk,f);
            remaining -= (long)chunk;
        }
    }
    int rc = sf_commit(&sf, !ferror(f));
    pdbw_abort(w);   /* closes+removes temp, frees index */
    return rc == 0 ? nrecs : -1;
}

int pdb_read_appinfo(const char *path, uint8_t *buf, int cap){
    sf_recover(path);
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
