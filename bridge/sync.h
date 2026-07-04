/* sync.h -- bridge sync engine interface. */
#ifndef SYNC_H
#define SYNC_H
#include "dav.h"
#include "appinfo.h"   /* CAT_COUNT */

/* record kind (numbered so legacy 0/1 callers still mean card/cal). */
enum { KIND_CARD=0, KIND_CAL=1, KIND_TODO=2 };

/* conflict = a record changed on BOTH sides since the last sync. */
typedef enum { POL_SERVER, POL_LOCAL, POL_BOTH } ConflictPolicy;

typedef struct {
    int pushNew, pushMod, pushDel;
    int pullNew, pullMod, pullDel;
    int conflicts, unchanged;
} SyncStats;

/* full-sync primitives (initial seed / debugging). kind: KIND_CAL/CARD/TODO. */
int sync_push(const DavCtx*,const char*pdbpath,const char*coll,int kind);
int sync_pull(const DavCtx*,const char*coll,const char*outpdb,int kind);

/* incremental, conflict-aware two-way sync of one collection.
 * Reads localpdb + state mapfile + server state, reconciles, performs the
 * DAV ops, writes the merged PDB to outpdb, and rewrites mapfile.
 * localpdb may be written back to outpdb == localpdb (same path is fine).   */
int sync_collection(const DavCtx*d,const char*localpdb,const char*outpdb,
                    const char*coll,int kind,const char*mapfile,
                    ConflictPolicy pol,SyncStats*st);

/* category-routed multi-collection sync: each Palm category id 0..15 routes to
 * coll[id] (or def when NULL). Records partition by category, each subset syncs
 * against its collection (own map file under mapdir), and the merged PDB is
 * written to outpdb with its AppInfo preserved. Pulled records are stamped with
 * the category that routes to the collection they came from.                  */
typedef struct { const char* coll[CAT_COUNT]; const char* def; } CatRoute;
int sync_categorized(const DavCtx*d,const char*localpdb,const char*outpdb,
                     int kind,const CatRoute*rt,const char*mapdir,
                     ConflictPolicy pol,SyncStats*st);

#endif
