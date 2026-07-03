/* sync.h -- bridge sync engine interface. */
#ifndef SYNC_H
#define SYNC_H
#include "dav.h"

/* conflict = a record changed on BOTH sides since the last sync. */
typedef enum { POL_SERVER, POL_LOCAL, POL_BOTH } ConflictPolicy;

typedef struct {
    int pushNew, pushMod, pushDel;
    int pullNew, pullMod, pullDel;
    int conflicts, unchanged;
} SyncStats;

/* full-sync primitives (initial seed / debugging) */
int sync_push(const DavCtx*,const char*pdbpath,const char*coll,int isCal);
int sync_pull(const DavCtx*,const char*coll,const char*outpdb,int isCal);

/* incremental, conflict-aware two-way sync of one collection.
 * Reads localpdb + state mapfile + server state, reconciles, performs the
 * DAV ops, writes the merged PDB to outpdb, and rewrites mapfile.
 * localpdb may be written back to outpdb == localpdb (same path is fine).   */
int sync_collection(const DavCtx*d,const char*localpdb,const char*outpdb,
                    const char*coll,int isCal,const char*mapfile,
                    ConflictPolicy pol,SyncStats*st);

#endif
