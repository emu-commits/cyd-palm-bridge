/* find.h -- global streaming search across the Palm PIM databases.
 *
 * Palm's "Find" scans every app for a substring and lists the hits. This is the
 * portable, host-testable core: stream a PDB one record at a time (never fully
 * resident), decode it for the given app, and invoke a callback per match with
 * a short snippet for the results list. The firmware's Find silkscreen button
 * drives this over all four DBs; the host test proves the matching.
 */
#ifndef FIND_H
#define FIND_H
#include <stdint.h>

/* which app a PDB holds (decides which fields are searched). */
enum { FIND_CAL, FIND_TODO, FIND_ADDR, FIND_MEMO };

typedef struct {
    int      app;            /* FIND_* of the PDB the hit came from      */
    uint32_t uid;            /* record uniqueID                          */
    char     snippet[96];    /* matching text, trimmed for a results row */
} FindHit;

typedef void (*find_hit_cb)(const FindHit *hit, void *ctx);

/* Search one PDB for `query` (case-insensitive, ASCII-folded substring). Calls
 * cb once per matching record. Returns the number of hits, or -1 on read error.
 * An empty/NULL query matches nothing (returns 0). */
int find_in_pdb(const char *path, int app, const char *query,
                find_hit_cb cb, void *ctx);

#endif
