/* palm.h -- the Palm database (PDB) container: streaming reader and writers.
 *
 * Design rule: the database is NEVER held in RAM. A PDB is streamed one record
 * at a time. (The Date Book, Address and To Do record codecs and the
 * CalDAV/CardDAV bridge that once shared this header were archived on
 * 2026-11-07: git tag archive/pim-caldav. Memos, the one database left, is
 * plain text and needs no codec.)
 *
 * This header is host/ESP32-agnostic: pure C99, no malloc-heavy containers,
 * fixed-size record buffers sized for the largest realistic Palm record.
 */
#ifndef PALM_H
#define PALM_H
#include <stdint.h>
#include <stddef.h>

#define PALM_REC_MAX 4096          /* max single packed record we handle   */
#define PALM_EPOCH_1904 2082844800u /* seconds from unix epoch to 1904 base */

/* ---- record index attributes (high nibble of the 8-byte index entry) ---- */
#define REC_ATTR_DELETE  0x80
#define REC_ATTR_DIRTY   0x40
#define REC_ATTR_BUSY    0x20
#define REC_ATTR_SECRET  0x10
#define REC_ATTR_CAT     0x0F      /* low nibble = category id              */

/* ================= big-endian byte helpers (68k on-disk order) ========== */
static inline uint16_t be16(const uint8_t*p){ return (uint16_t)((p[0]<<8)|p[1]); }
static inline uint32_t be32(const uint8_t*p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3]; }
static inline void put16(uint8_t*p,uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static inline void put32(uint8_t*p,uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

/* ============================ PDB container ============================= */
typedef struct {
    uint8_t  attr;
    uint32_t uniqueID;            /* 24-bit                                  */
    const uint8_t *data;          /* points into a reader-owned buffer       */
    int      len;
} PdbRec;

/* streaming reader: cb called once per record; DB never fully resident.
 * return nonzero from cb to stop early. returns record count or -1.        */
typedef int (*pdb_rec_cb)(const PdbRec *rec, int index, void *ctx);
int pdb_read(const char *path, pdb_rec_cb cb, void *ctx);

/* random-access read of the record at index `want` (as handed to pdb_read's
 * callback). Fills buf (up to cap) + optional attr/uid out-params. Returns len
 * (>=0) or -1. Lets the sync engine load one local record's bytes on demand
 * instead of holding every record in a big RAM arena.                       */
int pdb_read_one(const char *path, int want, uint8_t *buf, int cap,
                 uint8_t *attr, uint32_t *uid);

/* streaming writer: record bytes spill to a temp file as they are added; only
 * a tiny per-record index stays resident, so the output PDB is bounded by disk
 * rather than a fixed RAM arena. commit() sorts by uniqueID and assembles the
 * final file; abort() discards. One temp file per writer.                    */
typedef struct PdbW PdbW;
PdbW *pdbw_begin(const char *tmppath);
int   pdbw_rec(PdbW *w, uint32_t uid, uint8_t attr, const uint8_t *data, int len);
int   pdbw_count(const PdbW *w);
int   pdbw_commit(PdbW *w, const char *path, const char *name,
                  uint32_t type, uint32_t creator,
                  const uint8_t *appinfo, int ailen);   /* frees w */
void  pdbw_abort(PdbW *w);                               /* frees w */

/* writer: build a PDB from an array of records (records themselves are the
 * only sizable data; the array of PdbRec is small). type/creator are 4cc.  */
int pdb_write(const char *path, const char *name,
              uint32_t type, uint32_t creator,
              const PdbRec *recs, int nrecs);

/* writer variant that also emits an AppInfo block (category table etc.). */
int pdb_write_ai(const char *path, const char *name,
                 uint32_t type, uint32_t creator,
                 const uint8_t *appinfo, int ailen,
                 const PdbRec *recs, int nrecs);

/* read the AppInfo block bytes into buf (returns length, 0 if none, -1 err). */
int pdb_read_appinfo(const char *path, uint8_t *buf, int cap);

#endif
