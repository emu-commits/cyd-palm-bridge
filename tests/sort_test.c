/* sort_test.c -- the engine's sort, in RAM and in runs on the card.
 *
 * sync_sort_file() sorts an index file by its first field within a RAM budget:
 * one buffer when the file fits, sorted runs on the card and a K-way merge when
 * it doesn't. A wrong sort is the worst failure the engine has (the merge-join
 * then mis-pairs records into deletions and duplicates), so this checks it
 * against a plain in-memory sort on random files at many budgets: the keys
 * must come out in order, the lines must be exactly the same lines, and no run
 * file may be left on the card. Offline; part of `make test`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/sync.h"

#define F "state/.sort_test"
static int fails = 0;
static void CK(int c, const char *m){ if(!c){ fails++; printf("  FAIL: %s\n", m); } }

static unsigned rng = 12345;
static unsigned rnd(void){ rng = rng * 1103515245u + 12345u; return (rng >> 8) & 0xFFFFFF; }

/* the first field, as the engine compares it */
static int keycmp(const char *a, const char *b){
    for(;;){
        char x = *a, y = *b;
        int ex = (x=='\t'||x=='\n'||x==0), ey = (y=='\t'||y=='\n'||y==0);
        if(ex || ey) return ex == ey ? 0 : (ex ? -1 : 1);
        if(x != y) return (unsigned char)x - (unsigned char)y;
        a++; b++;
    }
}
static int strcmpp(const void *a, const void *b){ return strcmp(*(char *const *)a, *(char *const *)b); }

/* read every line (with its newline added if missing) */
static char **readLines(const char *p, int *n){
    FILE *f = fopen(p, "rb"); *n = 0; if(!f) return NULL;
    char **v = NULL; int cap = 0; char ln[1024];
    while(fgets(ln, sizeof ln, f)){
        size_t l = strlen(ln); if(!l || ln[l-1] != '\n'){ ln[l] = '\n'; ln[l+1] = 0; }
        if(*n == cap){ cap = cap ? cap * 2 : 64; v = realloc(v, (size_t)cap * sizeof *v); }
        v[(*n)++] = strdup(ln);
    }
    fclose(f); return v;
}
static void freeLines(char **v, int n){ for(int i = 0; i < n; i++) free(v[i]); free(v); }

static int leftovers(void){
    int n = 0;
    for(int p = 0; p < 8; p++) for(int r = 0; r < 64; r++){
        char rp[64]; snprintf(rp, sizeof rp, "state/.srt%d.%d", p, r);
        FILE *f = fopen(rp, "rb"); if(f){ fclose(f); n++; }
    }
    return n;
}

/* write n random lines in one of three key shapes; dup keys on purpose */
static void makeFile(int n, int shape, int lastNoNewline){
    FILE *f = fopen(F, "wb");
    for(int i = 0; i < n; i++){
        unsigned k = rnd() % (unsigned)(n / 2 + 1);
        if(shape == 0) fprintf(f, "%016llx\t%u\tname-%u.ics\tetag-%u\t%u", (unsigned long long)k * 2654435761ull, k, k, rnd(), rnd());
        else if(shape == 1) fprintf(f, "%010u\t%u\t%u\t%u\t%u", k, i, rnd() % 8, rnd() % 600, rnd());
        else fprintf(f, "%s%u.ics\t%u\t1", (k % 3) ? "ABCD-" : "palm-", k, rnd());
        if(i < n - 1 || !lastNoNewline) fputc('\n', f);
    }
    fclose(f);
}

static void one(int n, int shape, long budget, int lastNoNewline){
    makeFile(n, shape, lastNoNewline);
    int n0; char **before = readLines(F, &n0);
    sync_set_max_sort(budget);
    int ok = sync_sort_file(F);
    char m[160]; snprintf(m, sizeof m, "n=%d shape=%d budget=%ld: sorted", n, shape, budget);
    CK(ok == 1, m);
    int n1; char **after = readLines(F, &n1);
    snprintf(m, sizeof m, "n=%d shape=%d budget=%ld: same number of lines (%d vs %d)", n, shape, budget, n0, n1);
    CK(n0 == n1, m);
    int order = 1;
    for(int i = 1; i < n1; i++) if(keycmp(after[i-1], after[i]) > 0){ order = 0; break; }
    snprintf(m, sizeof m, "n=%d shape=%d budget=%ld: keys in order", n, shape, budget);
    CK(order, m);
    if(n0 == n1 && n0 > 0){
        qsort(before, n0, sizeof *before, strcmpp); qsort(after, n1, sizeof *after, strcmpp);
        int same = 1; for(int i = 0; i < n0; i++) if(strcmp(before[i], after[i])){ same = 0; break; }
        snprintf(m, sizeof m, "n=%d shape=%d budget=%ld: exactly the same lines", n, shape, budget);
        CK(same, m);
    }
    snprintf(m, sizeof m, "n=%d shape=%d budget=%ld: no run files left on the card", n, shape, budget);
    CK(leftovers() == 0, m);
    freeLines(before, n0); freeLines(after, n1);
}

int main(void){
    printf("== the engine's sort: in RAM, and in runs on the card ==\n");
    static const long budgets[] = { 0, 1 << 16, 4096, 1024, 600 };
    static const int sizes[] = { 0, 1, 2, 7, 50, 400, 3000 };
    for(int b = 0; b < (int)(sizeof budgets / sizeof *budgets); b++)
        for(int z = 0; z < (int)(sizeof sizes / sizeof *sizes); z++)
            for(int shape = 0; shape < 3; shape++)
                one(sizes[z], shape, budgets[b], (sizes[z] + shape) % 2);

    /* 3000 lines at 600 bytes a run is hundreds of runs: several merge passes */
    printf("== a line longer than the budget is refused, and leaves nothing behind ==\n");
    makeFile(200, 0, 0);
    sync_set_max_sort(32);
    CK(sync_sort_file(F) == 0, "a budget smaller than one line is refused");
    CK(sync_too_big_bytes() > 0, "and the size is reported");
    CK(leftovers() == 0, "no run files left after a refusal");

    printf("== an absent or empty file is already sorted ==\n");
    remove(F); sync_set_max_sort(0);
    CK(sync_sort_file(F) == 1, "absent");
    FILE *f = fopen(F, "wb"); fclose(f);
    CK(sync_sort_file(F) == 1, "empty");
    remove(F);

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
