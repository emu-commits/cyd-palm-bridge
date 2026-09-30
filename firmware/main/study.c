/* study.c -- a Study round's question queue. See study.h. */
#include "study.h"
#include "srs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "safefile.h"
#include "study_demo.h"

/* xorshift32: deterministic for a seed, so a test (or a bug report) replays */
static uint32_t rnd(StSession *s){
    uint32_t x = s->seed ? s->seed : 0x9E3779B9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s->seed = x;
    return x;
}

int st_begin(StSession *s, const uint16_t *items, const uint8_t *quiz, int n, uint32_t seed){
    memset(s, 0, sizeof *s);
    if(n < 0 || n > 16000) return -1;             /* the queue indexes are 16-bit */
    s->seed = seed;
    int nq = 0;
    for(int i = 0; i < n; i++) nq += ((quiz[i] & ST_MEANING) != 0) + ((quiz[i] & ST_READING) != 0);
    /* room for every question to be put back once without a realloc; a
     * question put back more often than that waits at the end instead */
    s->cap = (uint16_t)(nq * 2 > 0 ? nq * 2 : 1);
    s->q = malloc(sizeof(StQ) * s->cap);
    s->it = malloc(sizeof(StItem) * (n ? n : 1));
    if(!s->q || !s->it){ st_end(s); return -1; }
    for(int i = 0; i < n; i++){
        uint8_t m = quiz[i] & (ST_MEANING | ST_READING);
        if(!m) m = ST_MEANING;
        s->it[i] = (StItem){ items[i], m, SRS_EASY, 0 };
        if(m & ST_MEANING) s->q[s->nq++] = (StQ){ items[i], ST_MEANING, 0 };
        if(m & ST_READING) s->q[s->nq++] = (StQ){ items[i], ST_READING, 0 };
    }
    s->nit = (uint16_t)n;
    for(int i = s->nq - 1; i > 0; i--){           /* Fisher-Yates */
        int j = (int)(rnd(s) % (uint32_t)(i + 1));
        StQ t = s->q[i]; s->q[i] = s->q[j]; s->q[j] = t;
    }
    return 0;
}

void st_end(StSession *s){
    free(s->q); free(s->it);
    s->q = NULL; s->it = NULL;
    s->nq = s->pos = s->cap = s->nit = 0;
}

int st_current(const StSession *s, StQ *q){
    if(s->pos >= s->nq) return 0;
    *q = s->q[s->pos];
    return 1;
}

int st_left(const StSession *s){ return s->nq - s->pos; }

static int find_item(const StSession *s, uint16_t item){
    for(int i = 0; i < s->nit; i++) if(s->it[i].item == item) return i;
    return -1;
}

int st_answer(StSession *s, int grade, StItem *done){
    if(s->pos >= s->nq) return 0;
    StQ cur = s->q[s->pos];
    int k = find_item(s, cur.item);
    if(k < 0){ s->pos++; return 0; }
    s->can_undo = 1;
    s->undo_pos = s->pos;
    s->undo_idx = (uint16_t)k;
    s->undo_item = s->it[k];
    s->undo_ins = 0xFFFF;
    s->undo_completed = 0;
    s->pos++;
    s->answered++;
    StItem *it = &s->it[k];
    if(grade < it->grade) it->grade = (uint8_t)grade;
    if(grade == SRS_AGAIN){
        s->missed++;
        it->wrong = 1;
        /* back into the queue 2 to 5 questions on, or at the end if fewer
         * are left: soon enough to learn from, not so soon it's remembered */
        int left = s->nq - s->pos;
        int at = s->pos + (left < 2 ? left : 2 + (int)(rnd(s) % (uint32_t)((left < 5 ? left : 5) - 1)));
        if(s->nq < s->cap){
            memmove(&s->q[at + 1], &s->q[at], sizeof(StQ) * (s->nq - at));
            s->q[at] = cur;
            s->nq++;
            s->undo_ins = (uint16_t)at;
        } else {
            /* full (a question missed again and again): overwrite nothing,
             * just ask this one again now */
            s->pos--;
            s->undo_ins = 0xFFFE;
        }
        return 0;
    }
    it->todo &= (uint8_t)~cur.q;
    if(it->todo) return 0;
    s->items_done++;
    if(it->wrong) s->items_wrong++;
    s->undo_completed = 1;
    if(done) *done = *it;
    return 1;
}

int st_undo(StSession *s, uint16_t *item){
    if(!s->can_undo) return -1;
    s->can_undo = 0;
    StItem *it = &s->it[s->undo_idx];
    if(s->undo_completed){
        s->items_done--;
        if(it->wrong) s->items_wrong--;
    }
    if(s->undo_ins < 0xFFFE){
        memmove(&s->q[s->undo_ins], &s->q[s->undo_ins + 1], sizeof(StQ) * (s->nq - s->undo_ins - 1));
        s->nq--;
    }
    if(s->undo_ins != 0xFFFF) s->missed--;              /* it was a wrong answer */
    s->answered--;
    *it = s->undo_item;
    s->pos = s->undo_pos;
    if(item) *item = it->item;
    return s->undo_completed;
}

/* ---- the card ---------------------------------------------------------- */

static int file_exists(const char *p){ struct stat st; return stat(p, &st) == 0; }

int st_install_demo(const char *root, int force){
    char dir[96], path[128], mark[96];
    snprintf(dir, sizeof dir, "%s/study", root);
    snprintf(mark, sizeof mark, "%s/study/.demo", root);
    if(!force && file_exists(mark)) return 0;
    mkdir(dir, 0777);
    snprintf(dir, sizeof dir, "%s/study/demo-kanji", root);
    mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/course.srs", dir);
    SafeFile sf;
    if(!sf_open(&sf, path, "wb")) return -1;
    int ok = fwrite(study_demo_srs, 1, study_demo_size, sf.f) == study_demo_size;
    if(sf_commit(&sf, ok)) return -1;
    FILE *m = fopen(mark, "wb");
    if(m) fclose(m);
    return 1;
}

int st_remove_course(const char *root, const char *id){
    static const char *const FILES[] = { "course.srs", "progress.dat", "progress.log",
                                         "progress.dat.tmp", "course.srs.tmp" };
    char p[128];
    if(!id[0] || strchr(id, '/') || strstr(id, "..")) return -1;
    for(size_t i = 0; i < sizeof FILES / sizeof FILES[0]; i++){
        snprintf(p, sizeof p, "%s/study/%s/%s", root, id, FILES[i]);
        remove(p);
    }
    snprintf(p, sizeof p, "%s/study/%s", root, id);
    return rmdir(p) == 0 ? 0 : -1;
}

int st_list_courses(const char *root, char ids[][ST_ID_MAX], int max){
    char dir[96], path[96 + ST_ID_MAX + 16];
    snprintf(dir, sizeof dir, "%s/study", root);
    DIR *d = opendir(dir);
    if(!d) return 0;
    int n = 0;
    struct dirent *e;
    while((e = readdir(d)) && n < max){
        size_t len = strlen(e->d_name);
        if(e->d_name[0] == '.' || len >= ST_ID_MAX) continue;
        memcpy(ids[n], e->d_name, len + 1);
        snprintf(path, sizeof path, "%s/%s/course.srs", dir, ids[n]);
        if(!file_exists(path)) continue;
        n++;
    }
    closedir(d);
    for(int i = 1; i < n; i++)                     /* insertion sort: a handful */
        for(int j = i; j > 0 && strcmp(ids[j - 1], ids[j]) > 0; j--){
            char t[ST_ID_MAX];
            memcpy(t, ids[j], ST_ID_MAX); memcpy(ids[j], ids[j - 1], ST_ID_MAX); memcpy(ids[j - 1], t, ST_ID_MAX);
        }
    return n;
}

int st_last_get(const char *root, char id[ST_ID_MAX]){
    char p[96];
    snprintf(p, sizeof p, "%s/study/last.txt", root);
    id[0] = 0;
    FILE *f = fopen(p, "rb");
    if(!f) return 0;
    size_t n = fread(id, 1, ST_ID_MAX - 1, f);
    fclose(f);
    id[n] = 0;
    char *nl = strpbrk(id, "\r\n");
    if(nl) *nl = 0;
    return id[0] != 0;
}

void st_last_set(const char *root, const char *id){
    char p[96];
    snprintf(p, sizeof p, "%s/study/last.txt", root);
    SafeFile sf;
    if(!sf_open(&sf, p, "wb")) return;
    int ok = fputs(id, sf.f) >= 0;
    sf_commit(&sf, ok);
}
