/* safefile.h -- replace a file so that a crash can never leave it half-written.
 * Header-only (static inline), so every build that already compiles a writer
 * needs no new source file. SPDX-License-Identifier: MIT
 *
 * THE PROBLEM. Every durable file on the card used to be rewritten in place:
 * fopen(path, "wb") truncates it to nothing, then the new contents stream in.
 * A power cut, a flat battery or a watchdog reset during that window leaves a
 * short file or an empty one. For the three synced databases the mass-delete
 * guard can pull the server's copy back; Memo has no server copy, and
 * config.ini, the game saves and the habit history have none either.
 *
 * THE FIX. Write everything to `<path>.tmp`, flush it to the card, and only
 * then swap it into place. At every instant one COMPLETE version exists:
 *
 *   crash while writing .tmp      -> old file intact; the partial .tmp is junk
 *   crash after fsync, before swap-> old file intact (the new write is lost,
 *                                    which is the most a crash can cost)
 *   crash between remove and rename (FatFs only, see below)
 *                                 -> no <path>, a COMPLETE .tmp: promote it
 *
 * sf_recover() is what turns those states back into one file, and every
 * reader of a durable file calls it first. It trusts a .tmp only when the
 * real file is MISSING -- the one state a finished write can leave -- and
 * throws it away otherwise, because a .tmp beside an intact file is a write
 * that never finished.
 *
 * FatFs (the device's SD driver) will not rename onto an existing name, so
 * the swap there is remove-then-rename, and the gap between the two is the
 * third row above. POSIX rename() replaces atomically, and is tried first, so
 * the host gate exercises both paths (tests/safefile_test.c). */
#ifndef SAFEFILE_H
#define SAFEFILE_H
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    FILE *f;
    char  path[160];
    char  tmp[168];
} SafeFile;

static inline int sf_exists(const char *p){ struct stat st; return stat(p, &st) == 0; }

/* Make <path> whole again after a crash. Safe (and cheap: one or two stats)
 * to call before every read. Returns 1 if it promoted a .tmp. */
static inline int sf_recover(const char *path){
    char tmp[168];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if(!sf_exists(tmp)) return 0;
    if(sf_exists(path)){ remove(tmp); return 0; }    /* an unfinished write  */
    return rename(tmp, path) == 0;                    /* a finished one      */
}

/* Open the .tmp for writing. `mode` is "w" or "wb". NULL on failure. */
static inline FILE *sf_open(SafeFile *sf, const char *path, const char *mode){
    snprintf(sf->path, sizeof sf->path, "%s", path);
    snprintf(sf->tmp,  sizeof sf->tmp,  "%s.tmp", path);
    sf->f = fopen(sf->tmp, mode);
    return sf->f;
}

/* Throw the write away; the old file is untouched. */
static inline void sf_abort(SafeFile *sf){
    if(sf->f){ fclose(sf->f); sf->f = NULL; }
    remove(sf->tmp);
}

/* Finish the write and swap it in. 0 on success; on any failure the old file
 * is left as it was and the .tmp is removed. `ok` lets a caller that already
 * saw a failed fwrite hand the verdict in, so there is one exit path. */
static inline int sf_commit(SafeFile *sf, int ok){
    if(!sf->f) return -1;
    if(fflush(sf->f) != 0) ok = 0;
    if(ok && fsync(fileno(sf->f)) != 0) ok = 0;       /* on the card, not in a cache */
    if(fclose(sf->f) != 0) ok = 0;
    sf->f = NULL;
    if(!ok){ remove(sf->tmp); return -1; }
    if(rename(sf->tmp, sf->path) == 0) return 0;      /* POSIX: atomic replace */
    remove(sf->path);                                 /* FatFs: make room...   */
    if(rename(sf->tmp, sf->path) == 0) return 0;      /* ...and swap           */
    return -1;          /* the .tmp is complete and sf_recover() will promote it */
}

#endif
