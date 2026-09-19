/* gurupool.c -- see gurupool.h. Parses the card's habit pool in place. */
#include "gurupool.h"
#include "guru.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A pool is a few kilobytes of text. The cap is not about memory so much as
 * refusing to read something that is obviously not a habit list -- a stray
 * photo renamed onto the card should be rejected in one stat(), not streamed. */
#define GP_MAX_BYTES 32768
#define GP_NAME_MAX  30
/* The detail screen scrolls, so a long why is fine. This only catches a runaway
 * line -- a file saved without line endings, most likely -- where the useful
 * thing is an error naming the habit rather than one enormous screen. Must
 * match WHY_MAX in tools/gen_guru_pool.py; gurupool_test.c parses the real
 * guru_pool.txt through both, so a drift between them shows up there. */
#define GP_WHY_MAX   600

/* The arena: one allocation for the file text (fields are null-terminated in
 * place, so no string is ever copied) and one for the task array pointing into
 * it. They live and die together, which is what makes the ownership rule in
 * guru.h a single sentence. */
static char     *g_text;
static GuruTask *g_tasks;
static int       g_from_sd;
static char      g_err[128];

static void gp_free(void){
    free(g_text);  g_text  = 0;
    free(g_tasks); g_tasks = 0;
}

static int gp_fail(int line, const char *msg, const char *detail){
    if(line > 0) snprintf(g_err, sizeof g_err, "guru.txt: line %d: %s%s%s",
                          line, msg, detail ? " " : "", detail ? detail : "");
    else         snprintf(g_err, sizeof g_err, "guru.txt: %s%s%s",
                          msg, detail ? " " : "", detail ? detail : "");
    return -1;
}

const char *gurupool_error(void){ return g_err; }
int         gurupool_from_sd(void){ return g_from_sd; }

/* ---------------------------------------------------------------- parsing */
/* Trim in place and return the start. The end is trimmed by writing a NUL, so
 * the caller must own the bytes -- which it does: this only ever runs over the
 * file arena. */
static char *gp_trim(char *s, char *end){
    while(s < end && (*s == ' ' || *s == '\t')) s++;
    while(end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    *end = 0;
    return s;
}

/* Split one line into its four fields, trimmed, in place. Returns the field
 * count found, so a wrong count can be reported rather than silently accepted. */
static int gp_split(char *line, char *eol, char *out[4]){
    int n = 0;
    char *p = line;
    while(n < 4){
        char *bar = p;
        while(bar < eol && *bar != '|') bar++;
        out[n++] = gp_trim(p, bar);
        if(bar >= eol) return n;
        p = bar + 1;
    }
    /* Falling out of the loop means the fourth field was terminated by another
     * '|' rather than by the end of the line -- so there is a fifth. Reported as
     * a count rather than tolerated, because the alternative is silently
     * truncating somebody's why line at a pipe they meant to type. */
    return 5;
}

int gurupool_load(const char *path){
    g_err[0] = 0;
    if(!path) return gp_fail(0, "no path", 0);

    /* No file is not a failure worth reporting. Most cards have no pool on them
     * and the built-in list is the right answer; leaving the error empty is what
     * lets the About box tell those users how to make one, instead of showing
     * them a complaint about a file they never created. Only a file that EXISTS
     * and could not be used gets an explanation. */
    FILE *f = fopen(path, "rb");
    if(!f) return -1;

    if(fseek(f, 0, SEEK_END) != 0){ fclose(f); return gp_fail(0, "unreadable", 0); }
    long sz = ftell(f);
    if(sz < 0){ fclose(f); return gp_fail(0, "unreadable", 0); }
    if(sz == 0){ fclose(f); return gp_fail(0, "is empty", 0); }
    if(sz > GP_MAX_BYTES){
        fclose(f);
        return gp_fail(0, "is too large to be a habit list", 0);
    }
    rewind(f);

    char *text = (char *)malloc((size_t)sz + 1);
    if(!text){ fclose(f); return gp_fail(0, "no memory to read it", 0); }
    size_t got = fread(text, 1, (size_t)sz, f);
    fclose(f);
    text[got] = 0;

    GuruTask *tasks = (GuruTask *)calloc(GU_TASK_MAX, sizeof *tasks);
    if(!tasks){ free(text); return gp_fail(0, "no memory to parse it", 0); }

    /* Parsed in file order first; grouped by category afterwards. */
    int n = 0, lineno = 0;
    char *p = text;
    while(*p){
        char *eol = p;
        while(*eol && *eol != '\n') eol++;
        char *next = *eol ? eol + 1 : eol;
        lineno++;

        char *line = gp_trim(p, eol);
        p = next;
        if(!*line || *line == '#') continue;

        if(n >= GU_TASK_MAX){
            free(text); free(tasks);
            return gp_fail(lineno, "too many habits; the limit is 64", 0);
        }

        char *fld[4];
        int nf = gp_split(line, line + strlen(line), fld);
        if(nf != 4){
            free(text); free(tasks);
            return gp_fail(lineno, "expected 'id | category | name | why'", 0);
        }

        /* id */
        char *endp = 0;
        long id = strtol(fld[0], &endp, 10);
        if(!fld[0][0] || (endp && *endp) || id < 1 || id > GU_TASK_MAX){
            free(text); free(tasks);
            return gp_fail(lineno, "bad id", fld[0]);
        }
        for(int i = 0; i < n; i++) if(tasks[i].id == (uint16_t)id){
            free(text); free(tasks);
            return gp_fail(lineno, "duplicate id", fld[0]);
        }

        int cat = guru_cat_from_key(fld[1]);
        if(cat < 0){
            free(text); free(tasks);
            return gp_fail(lineno, "unknown category", fld[1]);
        }
        if(!fld[2][0]){
            free(text); free(tasks);
            return gp_fail(lineno, "no name", 0);
        }
        if(strlen(fld[2]) > GP_NAME_MAX){
            free(text); free(tasks);
            return gp_fail(lineno, "name is too long for the row", fld[2]);
        }
        if(!fld[3][0]){
            free(text); free(tasks);
            return gp_fail(lineno, "no why line", fld[2]);
        }
        if(strlen(fld[3]) > GP_WHY_MAX){
            free(text); free(tasks);
            return gp_fail(lineno, "why line is far too long for", fld[2]);
        }

        tasks[n].id   = (uint16_t)id;
        tasks[n].cat  = (uint8_t)cat;
        tasks[n].name = fld[2];
        tasks[n].why  = fld[3];
        n++;
    }

    if(n == 0){
        free(text); free(tasks);
        return gp_fail(0, "has no habits in it", 0);
    }

    /* Group by category, stable within each. The list screen starts a new
     * heading whenever the category changes as it walks the pool, so an
     * interleaved file would print "Gut" four times. Sorting here means the user
     * can write a habit down anywhere in their file and still find it under the
     * right heading -- the file is for editing, not for being careful in. */
    GuruTask *sorted = (GuruTask *)calloc((size_t)n, sizeof *sorted);
    if(!sorted){ free(text); free(tasks); return gp_fail(0, "no memory to sort it", 0); }
    int k = 0;
    for(int c = 0; c < GU_NCAT; c++)
        for(int i = 0; i < n; i++)
            if(tasks[i].cat == (uint8_t)c) sorted[k++] = tasks[i];
    free(tasks);

    if(guru_pool_set(sorted, n) != 0){
        free(text); free(sorted);
        return gp_fail(0, "was rejected as a pool", 0);
    }

    /* Only now is the old arena unreachable: guru.c pointed into it until the
     * line above. Freeing earlier would have left it borrowing freed memory for
     * the length of a failed parse. */
    gp_free();
    g_text    = text;
    g_tasks   = sorted;
    g_from_sd = 1;
    return n;
}

/* ---------------------------------------------------------------- exporting */
int gurupool_export(const char *path){
    g_err[0] = 0;
    if(!path) return -1;

    FILE *chk = fopen(path, "rb");
    if(chk){ fclose(chk); gp_fail(0, "already on the card; not overwritten", 0); return -1; }

    FILE *f = fopen(path, "wb");
    if(!f){ gp_fail(0, "could not be written", 0); return -1; }

    fprintf(f,
        "# Guru habit pool.\n"
        "#\n"
        "# Edit this file and Guru picks it up next time it starts. Delete it to\n"
        "# go back to the list built into the firmware.\n"
        "#\n"
        "# One habit per line:\n"
        "#\n"
        "#     id | category | name | why\n"
        "#\n"
        "#   id        1-64, and PERMANENT -- the log on this card stores it, so a\n"
        "#             record written today still means the same habit next year.\n"
        "#             Use the next free number for a new habit; never reuse one.\n"
        "#   category  %s, %s, %s, %s, %s\n"
        "#   name      up to %d characters or the row clips\n"
        "#   why       one or two sentences, shown when the row is tapped\n"
        "#\n"
        "# Blank lines and lines starting with # are ignored. The order here does\n"
        "# not matter; the list is grouped by category when it is drawn.\n"
        "#\n"
        "# NOT MEDICAL ADVICE. These are popular wellness habits. A why line can\n"
        "# say what something is for, but should not name a disease or give a dose.\n",
        guru_cat_key(0), guru_cat_key(1), guru_cat_key(2),
        guru_cat_key(3), guru_cat_key(4), GP_NAME_MAX);

    /* the BUILT-IN pool, deliberately: the export is a pristine starting point,
     * so exporting after a bad edit gives the user the original back. */
    int last = -1;
    for(int i = 0; i < GU_POOL_BUILTIN_N; i++){
        const GuruTask *t = &GU_POOL_BUILTIN[i];
        if((int)t->cat != last){
            fprintf(f, "\n# --- %s\n", guru_cat_name(t->cat));
            last = (int)t->cat;
        }
        fprintf(f, "%d | %s | %s | %s\n",
                (int)t->id, guru_cat_key(t->cat), t->name, t->why);
    }

    int bad = ferror(f);
    if(fclose(f) != 0 || bad){ gp_fail(0, "could not be written", 0); return -1; }
    return 0;
}
