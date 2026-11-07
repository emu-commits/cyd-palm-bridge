/* gurupool_test.c -- host gate for the habit pool's file format
 * (firmware/main/gurupool.c).
 *
 * The pool moved out of C and into firmware/main/guru_pool.txt so it can be
 * edited without a reflash. That is a real improvement and a new hazard: the
 * pool is now USER INPUT, arriving off a removable card, written in a text
 * editor of unknown habits. It will eventually be malformed. The contract this
 * file pins is that a bad pool never costs the user their app -- every rejection
 * leaves the built-in list installed and Guru working.
 *
 * It also closes the loop the generator opens: tools/gen_guru_pool.py compiles
 * guru_pool.txt into the built-in table, and gurupool.c parses the same grammar
 * at runtime. Two parsers, one format. The strongest test here is simply that
 * the real guru_pool.txt, loaded through the C parser, comes out identical to
 * the table Python generated from it -- if the two ever drift, this says so.
 *
 * Needs a writable temp dir; uses no /sdcard and no clock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "guru.h"
#include "gurupool.h"

static int fails = 0;
#define CK(c,m) do{ if(!(c)){ fails++; printf("  FAIL: %s\n",(m)); } else printf("  ok: %s\n",(m)); }while(0)

#define TMP "build/gurupool_tmp.txt"

/* write `body` to TMP and try to load it; returns what gurupool_load() said. */
static int load_text(const char *body){
    FILE *f = fopen(TMP, "wb");
    if(!f){ printf("  FAIL: cannot write %s\n", TMP); fails++; return -1; }
    fputs(body, f);
    fclose(f);
    return gurupool_load(TMP);
}

/* a minimal pool that touches every category, so guru_pool_set() accepts it */
#define ALLCATS \
    "1 | gut | A | why a\n" \
    "2 | metabolic | B | why b\n" \
    "3 | mind | C | why c\n" \
    "4 | strength | D | why d\n" \
    "5 | recovery | E | why e\n"

/* every rejection must leave the pool that was already installed alone */
static void reject(const char *body, const char *what){
    int before_n  = guru_ntasks();
    const GuruTask *before_0 = guru_task(0);
    int r = load_text(body);
    CK(r < 0, what);
    CK(guru_ntasks() == before_n && guru_task(0) == before_0,
       "  ...and the working pool was left alone");
}

int main(void){
    printf("== gurupool ==\n");

    /* ------------------------------------------------- the built-in default */
    {
        CK(GU_POOL_BUILTIN_N > 20,        "the built-in pool is deep enough");
        CK(guru_pool_is_custom() == 0,    "nothing is loaded, so the built-in pool is live");
        CK(guru_ntasks() == GU_POOL_BUILTIN_N, "and it is what guru_ntasks() reports");

        /* the list screen starts a new heading whenever the category changes as
         * it walks the pool, so a pool that revisits a category prints the same
         * heading twice. The generator sorts; this is the assertion that it did. */
        int seen[GU_NCAT];
        for(int c = 0; c < GU_NCAT; c++) seen[c] = 0;
        int revisits = 0, last = -1;
        for(int i = 0; i < GU_POOL_BUILTIN_N; i++){
            int c = GU_POOL_BUILTIN[i].cat;
            if(c != last){ if(seen[c]) revisits++; seen[c] = 1; last = c; }
        }
        CK(revisits == 0, "the built-in pool is grouped, so no heading repeats");
    }

    /* ------------------------------------------------ the category key table */
    {
        int bad = 0;
        for(int c = 0; c < GU_NCAT; c++){
            const char *k = guru_cat_key(c);
            if(!k[0] || guru_cat_from_key(k) != c) bad++;
        }
        CK(bad == 0, "every category key round-trips to its own index");
        CK(guru_cat_from_key("cardio") == -1, "an invented category is not a category");
        CK(guru_cat_from_key("")       == -1, "the empty string is not a category");
        CK(guru_cat_from_key(0)        == -1, "and neither is nothing at all");
        /* a near-miss must not match a prefix: "gu" is not "gut" */
        CK(guru_cat_from_key("gu")   == -1,   "a truncated key does not match");
        CK(guru_cat_from_key("gutt") == -1,   "nor does an overlong one");
    }

    /* ------------------------------------------------------- the real file
     * The loop-closing test: parse the file the built-in table was generated
     * from, and require the two to agree habit for habit. */
    {
        int n = gurupool_load("../firmware/main/guru_pool.txt");
        CK(n == GU_POOL_BUILTIN_N,
           "the real guru_pool.txt yields the same count as the generated table");
        if(n == GU_POOL_BUILTIN_N){
            int diff = 0;
            for(int i = 0; i < n; i++){
                const GuruTask *a = guru_task(i), *b = &GU_POOL_BUILTIN[i];
                if(a->id != b->id || a->cat != b->cat) diff++;
                else if(strcmp(a->name, b->name) || strcmp(a->why, b->why)) diff++;
            }
            CK(diff == 0,
               "and the C parser agrees with the Python generator, line for line");
        }
        CK(guru_pool_is_custom() == 1, "a loaded pool reports as custom");
        CK(gurupool_from_sd() == 1,    "...and as having come off the card");

        guru_pool_reset();
        CK(guru_pool_is_custom() == 0, "and it can be handed back to the built-in");
        CK(guru_ntasks() == GU_POOL_BUILTIN_N, "which restores the built-in count");
    }

    /* ------------------------------------------------------- a good card pool */
    {
        int n = load_text("# a comment\n"
                          "\n"
                          "   \n"
                          ALLCATS
                          "# trailing comment\n");
        CK(n == 5,                        "a small hand-written pool loads");
        CK(guru_ntasks() == 5,            "and replaces the built-in one entirely");
        CK(guru_task_by_id(3) != 0 &&
           strcmp(guru_task_by_id(3)->name, "C") == 0, "ids find their habit");
        CK(guru_task_by_id(9) == 0,       "an id not in the file finds nothing");
        CK(gurupool_error()[0] == 0,      "a good load reports no error");

        /* whitespace is what a text editor produces, not an error */
        n = load_text("  1|gut|A|why a  \n2 |  metabolic  | B |why b\n"
                      "3 | mind | C | why c\n4 | strength | D | why d\n"
                      "5 | recovery | E | why e\n");
        CK(n == 5, "fields are trimmed and the separators need no spaces");
        CK(strcmp(guru_task_by_id(1)->name, "A") == 0, "...leaving no stray spaces");

        /* CRLF: the file is meant to be edited, quite possibly on Windows */
        n = load_text("1 | gut | A | why a\r\n2 | metabolic | B | why b\r\n"
                      "3 | mind | C | why c\r\n4 | strength | D | why d\r\n"
                      "5 | recovery | E | why e\r\n");
        CK(n == 5, "a file saved with CRLF line endings loads");
        CK(strcmp(guru_task_by_id(1)->why, "why a") == 0,
           "...without a carriage return left on the end of the last field");

        /* a file that does not end in a newline is a normal thing to produce */
        n = load_text(ALLCATS "6 | gut | F | why f");
        CK(n == 6, "a missing final newline still yields the last habit");
    }

    /* ------------------------------------------------------------- grouping */
    {
        int n = load_text("1 | gut | A | why a\n"
                          "2 | recovery | E | why e\n"
                          "3 | gut | B | why b\n"
                          "4 | metabolic | C | why c\n"
                          "5 | mind | D | why d\n"
                          "6 | strength | F | why f\n");
        CK(n == 6, "a pool written in no particular order loads");
        CK(guru_task(0)->cat == GU_CAT_GUT && guru_task(1)->cat == GU_CAT_GUT,
           "and comes back grouped by category, not in file order");
        CK(guru_task(n - 1)->cat == GU_CAT_RECOV, "with recovery last");
        /* stable within a category: the file's order is the user's order */
        CK(guru_task(0)->id == 1 && guru_task(1)->id == 3,
           "habits keep their file order within a category");
    }

    /* ---------------------------------------------- everything a card can be
     * Each of these must be refused AND must leave the previous pool intact --
     * a malformed card costs the user nothing. The pool loaded above is still
     * live going in, which is what the second half of reject() checks. */
    {
        reject("", "an empty file is refused");
        reject("# only comments\n\n", "a file with no habits is refused");
        reject("1 | gut | A\n", "a line with too few fields is refused");
        reject("1 | gut | A | why | extra\n", "a line with too many is refused");
        reject("x | gut | A | why a\n", "a non-numeric id is refused");
        reject("0 | gut | A | why a\n", "id zero is refused (it indexes a bit)");
        reject("65 | gut | A | why a\n", "an id past the 64-bit ceiling is refused");
        reject("-1 | gut | A | why a\n", "a negative id is refused");
        reject("1 | gut | A | why a\n1 | mind | B | why b\n",
               "a duplicate id is refused, not silently merged");
        reject("1 | cardio | A | why a\n", "an unknown category is refused");
        reject("1 | gut |  | why a\n", "a habit with no name is refused");
        reject("1 | gut | A |  \n", "a habit with no why line is refused");
        reject("1 | gut | This name is comfortably past thirty characters | why\n",
               "a name too long for the row is refused");

        /* a file saved with no line endings arrives as one enormous why */
        {
            static char runaway[1200];
            int at = snprintf(runaway, sizeof runaway, "1 | gut | A | ");
            while(at < (int)sizeof runaway - 2) runaway[at++] = 'x';
            runaway[at++] = '\n';
            runaway[at]   = 0;
            reject(runaway, "a runaway why line is refused, not rendered");
        }
    }

    /* a missing file is the NORMAL case -- most cards have no pool on them */
    {
        int before = guru_ntasks();
        int r = gurupool_load("build/definitely-not-here.txt");
        CK(r < 0,                     "a card with no guru.txt reports nothing loaded");
        CK(guru_ntasks() == before,   "...and simply leaves the pool as it was");
        /* and it is NOT an error: About tells these users how to make one rather
         * than complaining about a file they never created. */
        CK(gurupool_error()[0] == 0,  "...and does not report a problem, because there isn't one");
        CK(gurupool_load(0) < 0,      "a null path is survivable");
    }

    /* ------------------------------------------------------------- exporting */
    {
        remove("build/gurupool_export.txt");
        CK(gurupool_export("build/gurupool_export.txt") == 0, "the built-in pool exports");

        /* the export has to be something the parser accepts, or the user's first
         * edit starts from a file that was already broken */
        guru_pool_reset();
        int n = gurupool_load("build/gurupool_export.txt");
        CK(n == GU_POOL_BUILTIN_N, "the exported file loads back as the same pool");

        CK(gurupool_export("build/gurupool_export.txt") == -1,
           "exporting over an existing pool is refused, not silently destructive");
        CK(gurupool_export("/nonexistent-dir/guru.txt") == -1,
           "an unwritable card reports failure rather than claiming success");
        remove("build/gurupool_export.txt");
    }

    /* -------------------------------------------- editing on the device */
    {
        const char *P = "build/gurupool_edit.txt";
        char bak[64]; snprintf(bak, sizeof bak, "%s.bak", P);
        remove(P); remove(bak);
        guru_pool_reset();
        int nb = guru_ntasks();
        const GuruTask *t1 = guru_task_by_id(1);
        int cat1 = t1 ? t1->cat : 0;

        /* an edit to the built-in list writes the whole list to the card */
        CK(gurupool_put(P, 1, cat1, "One brazil | nut", "Line one\nline two") == 1,
           "editing habit 1 of the built-in list saves");
        CK(gurupool_from_sd() && guru_ntasks() == nb, "...as a card list of the same size");
        t1 = guru_task_by_id(1);
        CK(t1 && !strcmp(t1->name, "One brazil / nut") && !strcmp(t1->why, "Line one line two"),
           "a '|' and a line break are cleaned out of the fields");

        /* a new habit takes the lowest free id */
        int free_id = 0;
        for(int k = 1; k <= GU_TASK_MAX && !free_id; k++) if(!guru_task_by_id(k)) free_id = k;
        int id = gurupool_put(P, 0, GU_CAT_RECOV, "Read a page", "Ten minutes of paper.");
        CK(id == free_id && guru_ntasks() == nb + 1, "a new habit gets the lowest free id");
        CK(guru_task_by_id(id) && guru_task_by_id(id)->cat == GU_CAT_RECOV, "...in its category");

        /* moving a habit to another category keeps its id */
        CK(gurupool_put(P, id, GU_CAT_GUT, "Read a page", "Ten minutes of paper.") == id
           && guru_task_by_id(id)->cat == GU_CAT_GUT && guru_ntasks() == nb + 1,
           "a habit moved to another category keeps its id, and is not duplicated");

        /* empty fields are refused and change nothing */
        CK(gurupool_put(P, 0, GU_CAT_GUT, "   ", "why") == -1 && guru_ntasks() == nb + 1,
           "a habit with no name is refused");
        CK(gurupool_put(P, 0, GU_CAT_GUT, "Name", "") == -1 && gurupool_error()[0],
           "a habit with no why is refused, and says so");

        /* deleting */
        CK(gurupool_remove(P, id) == 0 && !guru_task_by_id(id) && guru_ntasks() == nb,
           "deleting a habit removes it");
        CK(gurupool_remove(P, id) == -1, "deleting it twice is refused");
        CK(gurupool_load(P) == nb, "and the card file holds the same list");

        /* a card file that did not load is kept, not written over */
        load_text("this is not a habit list\n");
        FILE *f = fopen(P, "wb"); fputs("broken\n", f); fclose(f);
        guru_pool_reset();
        gurupool_load(P);                                  /* fails: built-in list */
        CK(!gurupool_from_sd(), "a broken card list leaves the built-in one");
        CK(gurupool_put(P, 1, cat1, "Brazil nut", "Selenium.") == 1, "an edit still saves");
        f = fopen(bak, "rb");
        char line[16] = "";
        if(f){ if(!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
        CK(!strcmp(line, "broken\n"), "...and the broken file is kept as guru.txt.bak");

        /* the last habit cannot go */
        CK(load_text(ALLCATS) == 5, "a five-habit list");
        for(int k = 1; k <= 4; k++) gurupool_remove(TMP, k);
        CK(guru_ntasks() == 1 && gurupool_remove(TMP, 5) == -1, "the last habit can't be deleted");

        /* guru_forget clears today's tick but keeps the count */
        GuruState g; guru_state_init(&g);
        guru_toggle(&g, 5, 1700000000u, 0);
        guru_forget(&g, 5);
        CK(!guru_is_checked(&g, 5, 1700000000u, 0) && guru_today_n(&g, 1700000000u, 0) == 1,
           "a deleted habit's tick goes, today's count stays");
        remove(P); remove(bak);
        guru_pool_reset();
    }

    remove(TMP);
    printf(fails ? "== gurupool: %d FAILURE(S) ==\n" : "== gurupool: all passed ==\n", fails);
    return fails ? 1 : 0;
}
