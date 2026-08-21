/* sync.h -- bridge sync engine interface. */
#ifndef SYNC_H
#define SYNC_H
#include <stddef.h>
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
 * localpdb may be written back to outpdb == localpdb (same path is fine).
 *
 * Returns the number of records kept, or a negative code. The codes are distinct
 * because the caller SHOWS the reason to the user, and "low memory" was being
 * printed for a full SD card:
 *   -1  out of memory (the scratch buffers or the per-run state)
 *   -2  guard refused to overwrite a non-empty PDB with an empty result
 *   -3  local file error -- the output temp would not open (SD full/absent/RO)
 *   -4  the transport went down mid-collection (see DAV_FAIL_LIMIT in dav.h).
 *       The partial merge is discarded and the local PDB is left untouched.
 *   -5  refused up front: the working set does not fit the declared budget.
 *       Nothing was allocated and no request was sent.
 *   -6  the collection is larger than this device can process: an in-RAM sort,
 *       or the merged output's index, would not fit. Local data is untouched
 *       and the map is NOT republished. Retrying will not help -- this is a
 *       size limit, not a transient shortage. See sortFile/keepBytes in sync.c
 *       for why this must never be allowed to proceed quietly.
 */
int sync_collection(const DavCtx*d,const char*localpdb,const char*outpdb,
                    const char*coll,int kind,const char*mapfile,
                    ConflictPolicy pol,SyncStats*st);

/* category-routed multi-collection sync: each Palm category id 0..15 routes to
 * coll[id] (or def when NULL). Records partition by category, each subset syncs
 * against its collection (own map file under mapdir), and the merged PDB is
 * written to outpdb with its AppInfo preserved. Pulled records are stamped with
 * the category that routes to the collection they came from.
 * Same negative return codes as sync_collection.                              */
typedef struct { const char* coll[CAT_COUNT]; const char* def; } CatRoute;
int sync_categorized(const DavCtx*d,const char*localpdb,const char*outpdb,
                     int kind,const CatRoute*rt,const char*mapdir,
                     ConflictPolicy pol,SyncStats*st);

/* ---- memory budget --------------------------------------------------------
 * The guardrail. This device has ~45 KB of heap free once Wi-Fi is up, and a
 * TLS handshake can happen at ANY point inside a collection -- sortFile drops
 * the connection deliberately, so a mid-collection reconnect is guaranteed
 * rather than incidental. The sync's working set therefore has to fit in what
 * is left after reserving for that handshake.
 *
 * It did not, by roughly 13 KB, and nothing checked. The shortfall surfaced as
 * whichever unrelated allocation happened to ask next -- a 4770-byte TLS buffer,
 * a 2.6 KB sort buffer -- four hundred seconds into a collection, which is how
 * it stayed misdiagnosed as "low memory" for so long.
 *
 * So the caller declares the ceiling and the engine refuses BEFORE any network
 * I/O if it cannot fit. sync_set_budget(0) means unlimited, which is what the
 * host build and the gates use. sync_working_set() is what scratch_alloc will
 * ask for, so a caller can report the shortfall in the same breath as refusing.
 */
void   sync_set_budget(size_t bytes);
size_t sync_working_set(void);
/* After a -6, the largest single allocation the collection asked for and did not
 * get. Zero if the limit was the output index rather than a sort. */
long   sync_too_big_bytes(void);
/* Ceiling on any single in-RAM sort the engine performs, in bytes. 0 (default)
 * means "whatever the allocator will give". Setting it makes the size limit a
 * decision rather than a discovery, and lets the gates exercise the refusal. */
void   sync_set_max_sort(long bytes);

/* ---- pull-only -------------------------------------------------------------
 * Refuse every write to the server: no PUT, no DELETE. The local PDB is still
 * updated from the server, so a sync is still useful; local changes simply wait.
 * Set while the engine's memory handling is being reworked, because a bug on a
 * half-converted path would damage the real account, which no reflash can undo.
 * Off by default -- the host build and the gates exercise full two-way sync. */
void sync_set_pull_only(int on);
int  sync_pull_only(void);

/* ---- heap-integrity checkpoints (temporary, for the memory rework) ---------
 * A heap overwrite does not fail where it happens; it fails in whatever
 * unrelated allocation next walks the corrupted free list, which on this device
 * was lwIP's receive path and then our own largest-free-block logging. That
 * tells you nothing about the culprit. The device installs a hook here that
 * checks heap integrity at each phase boundary, so the corruption can be
 * bracketed to a phase and then to a line. NULL (the default, and what the host
 * gates use) means no checks and no cost. */
typedef void (*SyncCheckFn)(const char *where);
void sync_set_check(SyncCheckFn fn);

/* Optional progress hook: the engine calls fn(done,total,ctx) once as each
 * collection starts (done=0) and once per reconciled record thereafter, so a
 * caller can drive an intra-collection progress indicator. `total` is the local
 * record count (a live estimate; server-only pulls can push done past it, so
 * clamp). Registered globally (not per-call) so the many sync_collection callers
 * are untouched; pass NULL to disable. Not thread-safe against a concurrent sync
 * -- set it before starting the sync task. */
typedef void (*SyncProgressFn)(int done,int total,void*ctx);
void sync_set_progress(SyncProgressFn fn,void*ctx);

/* Release the sync scratch buffers (~20 KB: emit body + one local record + the
 * server-object fetch buffer) that the sync entry points allocate on demand.
 * On the no-PSRAM device, call this after a HotSync so the RAM returns to the
 * interactive UI; the host CLI/tests may skip it (the process exits). Safe to
 * call when nothing is allocated (a no-op) and safe to call between syncs. */
void sync_free_scratch(void);

#endif
