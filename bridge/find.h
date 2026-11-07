/* find.h -- streaming search of the Memos database.
 *
 * Palm's "Find" scans for words that start with the query and lists the hits.
 * This is the portable, host-testable core: stream a PDB one record at a time
 * (never fully resident) and invoke a callback per match with a short snippet
 * for the results list. The firmware's Find silkscreen button drives it over
 * Memos, the one database left since Date Book, Address and To Do were
 * archived (2026-11-07); the host test proves the matching.
 */
#ifndef FIND_H
#define FIND_H
#include <stdint.h>

/* which app a PDB holds. Memos is the only one; the value is kept from when
 * there were four, so a FindHit reads the same. */
enum { FIND_MEMO = 3 };

typedef struct {
    int      app;            /* FIND_* of the PDB the hit came from      */
    uint32_t uid;            /* record uniqueID                          */
    char     snippet[96];    /* matching text, trimmed for a results row */
} FindHit;

typedef void (*find_hit_cb)(const FindHit *hit, void *ctx);

/* Search one PDB for `query` at the start of a word (case-insensitive,
 * ASCII-folded: "the" finds "Then", not "Lathe"). Calls
 * cb once per matching record. Returns the number of hits, or -1 on read error.
 * An empty/NULL query matches nothing (returns 0). */
int find_in_pdb(const char *path, int app, const char *query,
                find_hit_cb cb, void *ctx);

#endif
