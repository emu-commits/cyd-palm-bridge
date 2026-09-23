/* safefile_test.c -- the crash states of a file replacement, and the PDB layer
 * on top of them. SPDX-License-Identifier: MIT
 *
 * A crash cannot be caused on demand in a unit test, but its RESULTS can: each
 * point at which power could be lost leaves a particular pair of files on the
 * card, and every one of those pairs is built here by hand and handed to the
 * reader. What must come back is always one complete version of the file --
 * the old one or the new one, never a mixture and never nothing.
 *
 * The last block is the case that matters most: a MemoDB interrupted between
 * the two halves of the swap must still read as every memo, because Memo has
 * no server copy for the mass-delete guard to restore from. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../bridge/safefile.h"
#include "../bridge/palm.h"

static int fails;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } \
                    else printf("  ok: %s\n",(m)); }while(0)

#define P   "pdb/_sf.txt"
#define PT  "pdb/_sf.txt.tmp"

static void put(const char *p, const char *s){ FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int  is(const char *p, const char *s){
    char b[64] = {0}; FILE *f = fopen(p, "r"); if(!f) return 0;
    size_t n = fread(b, 1, sizeof b - 1, f); fclose(f); b[n] = 0;
    return !strcmp(b, s);
}

static int countCb(const PdbRec *r, int i, void *ctx){ (void)r; (void)i; (*(int *)ctx)++; return 0; }

int main(void){
    printf("safefile gate\n");
    remove(P); remove(PT);

    /* ---- a whole write ---- */
    put(P, "old");
    SafeFile sf; FILE *f = sf_open(&sf, P, "w");
    CK(f != NULL, "open writes beside the file");
    CK(is(P, "old"), "and leaves the live file alone while it does");
    fputs("new", f);
    CK(sf_commit(&sf, 1) == 0, "commit succeeds");
    CK(is(P, "new") && !sf_exists(PT), "the new version is in place and no .tmp is left");

    /* ---- a write that failed or was abandoned ---- */
    f = sf_open(&sf, P, "w"); fputs("half", f);
    CK(sf_commit(&sf, 0) != 0, "a failed write reports failure");
    CK(is(P, "new") && !sf_exists(PT), "and the old file survives it, whole");
    f = sf_open(&sf, P, "w"); fputs("half", f); sf_abort(&sf);
    CK(is(P, "new") && !sf_exists(PT), "an abort leaves the old file and no .tmp");

    /* ---- crash while the .tmp was being written: file + partial .tmp ---- */
    put(P, "old"); put(PT, "partial");
    CK(sf_recover(P) == 0, "recovery does not promote an unfinished write");
    CK(is(P, "old") && !sf_exists(PT), "it keeps the old file and discards the .tmp");

    /* ---- crash between remove and rename (FatFs): no file + complete .tmp ---- */
    remove(P); put(PT, "finished");
    CK(sf_recover(P) == 1, "recovery promotes the finished write");
    CK(is(P, "finished") && !sf_exists(PT), "which becomes the file");

    /* ---- nothing to do ---- */
    CK(sf_recover(P) == 0 && is(P, "finished"), "recovering a healthy file is a no-op");
    remove(P);
    CK(sf_recover(P) == 0 && !sf_exists(P), "and so is recovering a missing one");

    /* ---- the PDB layer: the case with no server copy ---- */
    #define DB  "pdb/_sf_memo.pdb"
    #define DBT "pdb/_sf_memo.pdb.tmp"
    remove(DB); remove(DBT);
    static const uint8_t body[] = "a memo";
    PdbRec recs[3];
    for(int i = 0; i < 3; i++){
        recs[i].attr = 0; recs[i].uniqueID = 100u + (uint32_t)i;
        recs[i].data = body; recs[i].len = (int)sizeof body;
    }
    CK(pdb_write(DB, "MemoDB", 0x44415441, 0x6D656D6F, recs, 3) == 3, "pdb_write goes through the swap");
    CK(!sf_exists(DBT), "and leaves no .tmp behind");
    /* now stage the FatFs crash: the live DB gone, the new one complete as .tmp */
    rename(DB, DBT);
    int n = 0;
    CK(pdb_read(DB, countCb, &n) == 3 && n == 3,
       "a MemoDB caught mid-swap still reads as all three memos");
    CK(sf_exists(DB) && !sf_exists(DBT), "and is a normal file again afterwards");
    /* and the other crash: a torn .tmp beside the intact database */
    put(DBT, "garbage");
    n = 0;
    CK(pdb_read(DB, countCb, &n) == 3 && n == 3 && !sf_exists(DBT),
       "a torn .tmp beside it is thrown away, not read");
    remove(DB);

    printf(fails ? "== safefile: %d FAILURE(S) ==\n" : "== safefile: all passed ==\n", fails);
    return fails ? 1 : 0;
}
