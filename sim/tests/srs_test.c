/* srs_test.c -- host gate for Study's schedulers and progress (firmware/main/srs.c).
 *
 *   srs_test                     every check below (make -C sim srs)
 *   srs_test sm2trace TZ         print the SM-2 year trace, for comparing with
 *                                an oracle by hand (see sm2_year)
 *
 * Run from sim/ (the course paths are relative to it); it works in
 * build/srs_tmp/. Built with ASan and UBSan. */
#include "srs.h"
#include "study.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEMO     "../courses/demo-kanji/course.srs"
#define CARDS    "../tests/data/study/cards/course.srs"
#define FEATURES "../tests/data/study/features/course.srs"
#define REMAP1   "../tests/data/study/remap-v1/course.srs"
#define REMAP2   "../tests/data/study/remap-v2/course.srs"
#define TMP      "build/srs_tmp"

static int fails;
#define CHECK(cond, ...) do { if(!(cond)){ fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while(0)

#define H 3600u
#define D 86400u
static const uint32_t T0 = 1767571200u;    /* 2026-01-05 00:00 UTC, a Monday */

static void fresh_dir(void){
    mkdir("build", 0777);
    mkdir(TMP, 0777);
    remove(TMP "/progress.dat");
    remove(TMP "/progress.dat.tmp");
    remove(TMP "/progress.log");
}

static Course *open_course(const char *path){
    Course *c = malloc(sizeof *c);
    if(course_open(c, path)){ printf("FAIL: open %s: %s\n", path, c->err); exit(1); }
    return c;
}

/* the item number of the item whose TERM is `term` and whose kind's name
 * starts with `kind` (NULL: any) -- the demo's radical 一 and kanji 一 share
 * a term */
static uint32_t item_of_kind(Course *c, const char *term, const char *kind){
    static uint8_t buf[COURSE_ITEM_BUF];
    for(uint32_t n = 0; n < c->n_items; n++){
        CourseItem it;
        if(course_item(c, n, &it) || course_item_load(c, &it, buf, sizeof buf) < 0) continue;
        if(kind && strncmp(c->kinds[it.kind].name, kind, strlen(kind))) continue;
        uint32_t pos = 0;
        CourseField f;
        if(course_field_next(buf, it.text_len, &pos, &f) == 1 && f.len == strlen(term) && !memcmp(f.text, term, f.len))
            return n;
    }
    printf("FAIL: no item %s\n", term);
    exit(1);
}
static uint32_t item_of(Course *c, const char *term){ return item_of_kind(c, term, NULL); }
static uint32_t kanji(Course *c, const char *term){ return item_of_kind(c, term, "kanji"); }
static uint32_t radical(Course *c, const char *term){ return item_of_kind(c, term, "radical"); }
static uint32_t word(Course *c, const char *term){ return item_of_kind(c, term, "word"); }

static uint32_t id_of(Course *c, uint32_t n){ CourseItem it; course_item(c, n, &it); return it.id; }

/* ---- the stages scheduler: the demo's, exactly ---- */

static void stages(void){
    Course *c = open_course(DEMO);
    SrsRec r;
    uint32_t now = T0 + 9 * H;
    /* always right, reviewed the moment it's due: 2h, 4h, 8h, 1d, 2d, 1w,
     * 2w, 30d, 120d, then retired -- 4,190 hours after the lesson */
    static const uint32_t steps[] = { 2*H, 4*H, 8*H, D, 2*D, 7*D, 14*D, 30*D, 120*D };
    srs_start(c, &r, 42, now, 0);
    CHECK(r.stage == 1 && r.due == now + 2 * H && r.flags == SRS_F_STARTED && !srs_known(c, &r), "the lesson");
    uint32_t t = now;
    for(int i = 0; i < 9; i++){
        t = r.due;
        srs_grade(c, &r, SRS_GOOD, t, 0);
        if(i < 8) CHECK(r.stage == i + 2 && r.due == t + steps[i + 1], "right %d: stage %u due +%u", i + 1, r.stage, r.due - t);
        CHECK(srs_known(c, &r), "known from stage 2 (right %d)", i + 1);
    }
    CHECK(r.stage == 10 && (r.flags & SRS_F_RETIRED) && r.due == 0 && r.reviews == 9 && r.lapses == 0, "retired");
    CHECK(t - now == 4190 * H, "the last review %u h after the lesson", (t - now) / H);
    srs_grade(c, &r, SRS_AGAIN, t + D, 0);
    CHECK(r.stage == 10 && r.due == 0 && r.reviews == 10, "a retired item stays retired");

    /* wrong answers: down one stage below 6, two from 6 up, never below 1 */
    srs_start(c, &r, 42, now, 0);
    srs_grade(c, &r, SRS_AGAIN, now, 0);
    CHECK(r.stage == 1 && r.due == now + 2 * H && r.lapses == 1 && r.reviews == 1, "wrong at stage 1");
    for(int i = 0; i < 6; i++) srs_grade(c, &r, SRS_GOOD, now, 0);
    CHECK(r.stage == 7, "stage 7");
    srs_grade(c, &r, SRS_AGAIN, now, 0);
    CHECK(r.stage == 5 && r.due == now + 2 * D && r.lapses == 2 && srs_known(c, &r), "wrong at 7 drops two, to 5");
    srs_grade(c, &r, SRS_AGAIN, now, 0);
    CHECK(r.stage == 4 && r.due == now + D, "wrong at 5 drops one, to 4");
    srs_grade(c, &r, SRS_AGAIN, now, 0);
    srs_grade(c, &r, SRS_AGAIN, now, 0);
    srs_grade(c, &r, SRS_AGAIN, now, 0);
    CHECK(r.stage == 1 && !srs_known(c, &r), "down to 1, not known");
    /* the four-button grades other than Again all count as right */
    srs_grade(c, &r, SRS_HARD, now, 0);
    srs_grade(c, &r, SRS_EASY, now, 0);
    CHECK(r.stage == 3, "Hard and Easy move up");
    /* groups: Learning 1, Known 2-5, Strong 6-7, Deep 8-9, Retired 10 */
    static const uint8_t want[11] = { 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4 };
    for(uint8_t st = 1; st <= 10; st++){ r.stage = st; CHECK(srs_group(c, &r) == want[st], "group of stage %u", st); }
    /* pack and unpack */
    SrsRec a = { 0xA1B2C3D4u, 0x01020304u, 0xBEEF, 7, 9, 3, 200, 65535 }, b;
    uint8_t buf[SRS_REC_SIZE];
    srs_pack(&a, buf);
    srs_unpack(buf, &b);
    CHECK(!memcmp(&a, &b, sizeof a) && buf[0] == 0xD4 && buf[3] == 0xA1, "pack/unpack, little-endian");
    course_close(c); free(c);
}

/* ---- the sm2 scheduler, against the web app it copies ----
 *
 * Five items, each with its own fixed pattern of grades, reviewed at 09:00
 * and 09:30 local time every day for a year whenever due. The expected
 * traces come from the Albanian course's own web scheduler (app/js/srs.js),
 * run through the same year under node in UTC and in UTC+2 (2026-09-30):
 * 551 reviews each, compared here by CRC-32 of the text. The oracle isn't
 * in this repo; to compare again, print this side with `srs_test sm2trace
 * 0` (or 7200) and diff it against the oracle's output. Line format: item
 * day minute grade ease*100 interval-days due.
 *
 * ONE DELIBERATE DIFFERENCE: the web app keeps ease as a JavaScript float,
 * which drifts (1.35 + 0.15 is 1.4999999999999998), so 13 days x 1.5 rounds
 * to 19 there and to 20 here. With its ease rounded to hundredths after
 * each review, and nothing else changed, the web app's trace is these
 * CRCs exactly. Unaltered, it first differs at item 4's 22nd review. */
#define SM2_REVIEWS  551
#define SM2_CRC_UTC  0x51d9d575u
#define SM2_CRC_P2   0x7eb61a23u

static uint32_t sm2_year(Course *c, int32_t tz, FILE *print, int *lines){
    static const uint8_t P0[] = { 2,2,3,2,1,2,0,2,2,2,3,0,1,2 }, P1[] = { 1 }, P2[] = { 0,2 },
                         P3[] = { 3,3,0 }, P4[] = { 2,0,2,2,1,3,2,0 };
    static const uint8_t *PATS[] = { P0, P1, P2, P3, P4 };
    static const int LENS[] = { sizeof P0, sizeof P1, sizeof P2, sizeof P3, sizeof P4 };
    uint32_t crc = 0;
    *lines = 0;
    for(int i = 0; i < 5; i++){
        SrsRec r;
        int k = 0, started = 0;
        for(int d = 0; d < 365; d++) for(int m = 0; m <= 30; m += 30){
            uint32_t now = (uint32_t)((int64_t)T0 + d * (int64_t)D + 9 * H + m * 60 - tz);
            if(!started){ srs_start(c, &r, 1, now, tz); started = 1; }
            if(r.due > now) continue;
            int g = PATS[i][k++ % LENS[i]];
            srs_grade(c, &r, g, now, tz);
            char line[96];
            int n = snprintf(line, sizeof line, "%s%d %d %d %d %d %u %u", *lines ? "\n" : "", i, d, m, g,
                             (int)SRS_EASE(r.ease), r.ivl_h / 24, r.due);
            crc = course_crc32(crc, line, (uint32_t)n);
            if(print) fputs(line, print);
            (*lines)++;
        }
    }
    if(print) fputc('\n', print);
    return crc;
}

static void sm2(void){
    Course *c = open_course(CARDS);
    int lines;
    uint32_t crc = sm2_year(c, 0, NULL, &lines);
    CHECK(lines == SM2_REVIEWS && crc == SM2_CRC_UTC, "sm2 year, UTC: %d reviews, crc %08x", lines, (unsigned)crc);
    crc = sm2_year(c, 7200, NULL, &lines);
    CHECK(lines == SM2_REVIEWS && crc == SM2_CRC_P2, "sm2 year, UTC+2: %d reviews, crc %08x", lines, (unsigned)crc);
    /* the rules, spelled out once */
    SrsRec r;
    uint32_t now = T0 + 9 * H;
    srs_start(c, &r, 1, now, 0);
    CHECK(r.stage == 0 && SRS_EASE(r.ease) == 250 && r.due == now, "a new item is due at once, ease 2.50");
    srs_grade(c, &r, SRS_GOOD, now, 0);
    CHECK(r.stage == 1 && r.ivl_h == 24 && r.due == T0 + 2 * D, "Good: 1 day, due the start of the day after");
    srs_grade(c, &r, SRS_AGAIN, now, 0);
    CHECK(r.stage == 0 && r.ivl_h == 0 && SRS_EASE(r.ease) == 230 && r.due == now + 600 && r.lapses == 1,
          "Again: 10 min, ease -0.20");
    for(int i = 0; i < 300; i++) srs_grade(c, &r, SRS_AGAIN, now, 0);
    CHECK(SRS_EASE(r.ease) == 130 && r.lapses == 255, "ease floors at 1.30; lapses saturate");
    for(int i = 0; i < 100; i++) srs_grade(c, &r, SRS_EASY, now, 0);
    CHECK(SRS_EASE(r.ease) == 1405 && r.ivl_h == 2730 * 24, "ease tops out at 14.05; the interval saturates");
    course_close(c); free(c);
}

/* ---- progress files ---- */

static void put(Srs *s, uint32_t n, SrsRec *r){
    if(srs_put(s, n, r, T0)) { printf("FAIL: put: %s\n", s->err); fails++; }
}

static long slurp(const char *p, uint8_t *b, long cap){
    FILE *f = fopen(p, "rb");
    if(!f) return -1;
    long n = (long)fread(b, 1, cap, f);
    fclose(f);
    return n;
}
static void spit(const char *p, const uint8_t *b, long n){
    FILE *f = fopen(p, "wb");
    fwrite(b, 1, n, f);
    fclose(f);
}

/* The log cut at every byte: the fold keeps every whole entry before the cut
 * and loses at most the one it cuts. */
static void log_truncation(void){
    Course *c = open_course(DEMO);
    Srs s;
    fresh_dir();
    CHECK(srs_open(&s, c, TMP, T0, 0) == 0, "a first open: %s", s.err);
    CHECK(s.started == 0 && s.n_due == 0, "nothing yet");
    srs_close(&s);
    static uint8_t dat0[4096], log[4096];
    long dat0_n = slurp(TMP "/progress.dat", dat0, sizeof dat0);
    CHECK(dat0_n == SRS_DAT_HEADER, "an empty progress.dat is its header");

    /* a session: 5 lessons, then grades; 12 entries */
    SrsRec hist[12];
    uint32_t items[12];
    int nh = 0;
    CHECK(srs_open(&s, c, TMP, T0, 0) == 0, "open");
    for(uint32_t n = 0; n < 5; n++){
        srs_start(c, &hist[nh], id_of(c, n), T0, 0);
        items[nh] = n; put(&s, n, &hist[nh]); nh++;
    }
    for(int i = 0; i < 7; i++){
        uint32_t n = (uint32_t)(i % 3);
        SrsRec r;
        CHECK(srs_get(&s, n, &r) == 1, "get");
        srs_grade(c, &r, i == 4 ? SRS_AGAIN : SRS_GOOD, T0 + (uint32_t)i * H, 0);
        hist[nh] = r; items[nh] = n; put(&s, n, &r); nh++;
    }
    srs_close(&s);                                  /* leaves the log unfolded */
    long log_n = slurp(TMP "/progress.log", log, sizeof log);
    CHECK(log_n == 12 * SRS_LOG_ENTRY, "12 entries of 20 bytes: %ld", log_n);

    for(long cut = 0; cut <= log_n; cut++){
        spit(TMP "/progress.dat", dat0, dat0_n);
        spit(TMP "/progress.log", log, cut);
        if(srs_open(&s, c, TMP, T0, 0)){ CHECK(0, "open with the log cut at %ld: %s", cut, s.err); continue; }
        int whole = (int)(cut / SRS_LOG_ENTRY);
        for(uint32_t n = 0; n < 5; n++){
            SrsRec want = { 0 }, got;
            int have = 0;
            for(int k = 0; k < whole; k++) if(items[k] == n){ want = hist[k]; have = 1; }
            int r = srs_get(&s, n, &got);
            CHECK(r == have && (!have || !memcmp(&want, &got, sizeof got)),
                  "cut at %ld: item %u %s", cut, n, have ? "has the wrong record" : "has a record it shouldn't");
        }
        CHECK(s.started == (uint32_t)(whole < 5 ? whole : 5), "cut at %ld: %u started", cut, s.started);
        srs_close(&s);
        struct stat st;
        CHECK(stat(TMP "/progress.log", &st) == 0 && st.st_size == 0, "cut at %ld: the log was emptied", cut);
    }

    /* a crash after the swap and before the truncate: the same log folds
     * twice, and nothing changes */
    spit(TMP "/progress.dat", dat0, dat0_n);
    spit(TMP "/progress.log", log, log_n);
    CHECK(srs_open(&s, c, TMP, T0, 0) == 0, "fold once");
    srs_close(&s);
    static uint8_t once[4096], twice[4096];
    long once_n = slurp(TMP "/progress.dat", once, sizeof once);
    spit(TMP "/progress.log", log, log_n);
    CHECK(srs_open(&s, c, TMP, T0, 0) == 0, "fold again");
    srs_close(&s);
    long twice_n = slurp(TMP "/progress.dat", twice, sizeof twice);
    CHECK(once_n == twice_n && !memcmp(once, twice, once_n), "folding twice is folding once");
    CHECK(once_n == SRS_DAT_HEADER + 5 * SRS_REC_SIZE, "5 records");

    /* a crash in safefile's remove-then-rename gap: only the .tmp is left */
    rename(TMP "/progress.dat", TMP "/progress.dat.tmp");
    CHECK(srs_open(&s, c, TMP, T0, 0) == 0 && s.started == 5, "the .tmp is promoted: %s", s.err);
    srs_close(&s);

    /* damage in progress.dat: refused, and left exactly as it was */
    static uint8_t bad[4096], after[4096];
    long bad_n = slurp(TMP "/progress.dat", bad, sizeof bad);
    for(long at = 0; at < bad_n; at += 7){
        uint8_t keep = bad[at];
        bad[at] ^= 0x10;
        spit(TMP "/progress.dat", bad, bad_n);
        spit(TMP "/progress.log", log, 0);
        int r = srs_open(&s, c, TMP, T0, 0);
        if(!r){
            /* only the fingerprint field is outside the CRC: a flip there
             * means "the course changed", which is a harmless remap */
            CHECK(at >= 12 && at < 16, "a flip at %ld went unnoticed", at);
            srs_close(&s);
        } else {
            CHECK(r == SRS_EBAD, "a flip at %ld: %d %s", at, r, s.err);
            long n = slurp(TMP "/progress.dat", after, sizeof after);
            CHECK(n == bad_n && !memcmp(after, bad, n), "a damaged progress.dat is left alone (flip at %ld)", at);
        }
        bad[at] = keep;
    }
    course_close(c); free(c);
}

/* A course update: items removed, added and reordered keep their progress
 * by id. */
static void remap(void){
    Course *v1 = open_course(REMAP1), *v2 = open_course(REMAP2);
    Srs s;
    fresh_dir();
    CHECK(srs_open(&s, v1, TMP, T0, 0) == 0, "open v1");
    const char *terms = "ABCDEF";
    for(int i = 0; i < 6; i++){
        uint32_t n = item_of(v1, (char[]){ terms[i], 0 });
        SrsRec r;
        srs_start(v1, &r, id_of(v1, n), T0, 0);
        for(int k = 0; k < i % 3; k++) srs_grade(v1, &r, SRS_GOOD, T0, 0);
        r.reviews = (uint16_t)(100 + i);                /* a mark to find it by */
        put(&s, n, &r);
    }
    srs_close(&s);
    static uint8_t log[1024];
    long log_n = slurp(TMP "/progress.log", log, sizeof log);
    CHECK(srs_open(&s, v1, TMP, T0, 0) == 0 && s.started == 6, "v1's progress folded");
    srs_close(&s);
    CHECK(srs_open(&s, v2, TMP, T0, 0) == 0, "open v2: %s", s.err);
    CHECK(s.dropped == 1 && s.remapped == 5 && s.started == 5, "c dropped, 5 kept: %u %u %u", s.dropped, s.remapped, s.started);
    for(int i = 0; i < 6; i++){
        if(terms[i] == 'C') continue;
        uint32_t n = item_of(v2, (char[]){ terms[i], 0 });
        SrsRec r;
        CHECK(srs_get(&s, n, &r) == 1 && r.reviews == 100 + i && r.stage == 1 + i % 3, "%c kept its progress", terms[i]);
    }
    SrsRec r;
    CHECK(srs_get(&s, item_of(v2, "G"), &r) == 0, "G is new: no record");
    uint16_t out[8], level;
    CHECK(srs_lessons(&s, out, 8, &level) == 1 && out[0] == item_of(v2, "G"), "G is the one lesson");
    srs_close(&s);
    CHECK(srs_open(&s, v2, TMP, T0, 0) == 0 && s.remapped == 0 && s.dropped == 0, "a second open doesn't remap");
    srs_close(&s);
    /* the update arrives before v1's session was ever folded: the log's
     * records carry over the same way, and c's is dropped */
    fresh_dir();
    CHECK(srs_open(&s, v1, TMP, T0, 0) == 0, "v1 again");
    srs_close(&s);
    spit(TMP "/progress.log", log, log_n);
    CHECK(srs_open(&s, v2, TMP, T0, 0) == 0 && s.started == 5 && s.dropped == 1, "from the log: 5 kept, c dropped (%u, %u)",
          s.started, s.dropped);
    for(int i = 0; i < 6; i++){
        if(terms[i] == 'C') continue;
        SrsRec q;
        CHECK(srs_get(&s, item_of(v2, (char[]){ terms[i], 0 }), &q) == 1 && q.reviews == 100 + i, "%c from the log", terms[i]);
    }
    srs_close(&s);
    course_close(v1); course_close(v2); free(v1); free(v2);
}

/* The demo's unlocks: radicals first; a kanji once its radicals are known; a
 * word once its kanji are; level 2 once 11 of level 1's 12 kanji are known. */
static int has(const uint16_t *v, int n, uint32_t x){ for(int i = 0; i < n; i++) if(v[i] == x) return 1; return 0; }

/* a lesson, then `rights` right answers */
static void learn(Srs *s, Course *c, uint32_t n, int rights){
    SrsRec r;
    srs_start(c, &r, id_of(c, n), T0, 0);
    for(int i = 0; i < rights; i++) srs_grade(c, &r, SRS_GOOD, T0 + 2 * H, 0);
    put(s, n, &r);
}

static void unlocks(void){
    Course *c = open_course(DEMO);
    Srs s;
    fresh_dir();
    CHECK(srs_open(&s, c, TMP, T0, 0) == 0, "open");
    uint16_t v[100], level;
    int n = srs_lessons(&s, v, 100, &level);
    CHECK(n == 8 && level == 1, "at first: the 8 level-1 radicals (%d), level %u", n, level);
    for(int i = 0; i < n; i++) CHECK(c->kinds[0].name[0] == 'r' && v[i] < 8, "a radical first");
    for(int i = 0; i < 8; i++) learn(&s, c, v[i], 0);
    CHECK(srs_lessons(&s, v, 100, &level) == 0, "radicals learning, not known: no kanji yet");
    for(uint32_t i = 0; i < 8; i++) learn(&s, c, i, 1);         /* one right answer each */
    n = srs_lessons(&s, v, 100, &level);
    CHECK(n == 12 && level == 1, "every level-1 kanji: %d", n);
    /* 大 is built from 一 and 人: known now. Words wait for their kanji. */
    CHECK(has(v, n, kanji(c, "大")) && !has(v, n, word(c, "大人")), "kanji before words");
    static const char *K1[] = { "一","二","三","十","人","口","大","山","日","月","木","本" };
    for(int i = 0; i < 10; i++) learn(&s, c, kanji(c, K1[i]), 1);
    n = srs_lessons(&s, v, 100, &level);
    CHECK(level == 1, "10 of 12 kanji known: level 2 still shut (%u)", level);
    CHECK(has(v, n, word(c, "大人")) && has(v, n, word(c, "一つ")) && !has(v, n, word(c, "日本")),
          "words whose kanji are known (大人, 一つ), not 日本 (本 isn't)");
    learn(&s, c, kanji(c, K1[10]), 1);
    n = srs_lessons(&s, v, 100, &level);
    CHECK(level == 2, "11 of 12 (90 %%, rounded up): level 2 opens (%u)", level);
    /* level 2: its radicals, and the kanji built only from level-1 radicals */
    CHECK(has(v, n, radical(c, "亻")) && has(v, n, radical(c, "卜")), "level 2's radicals");
    CHECK(has(v, n, kanji(c, "明")) && has(v, n, kanji(c, "林")) && has(v, n, kanji(c, "森")),
          "明 (日+月), 林 and 森 (木) open with the level");
    CHECK(!has(v, n, kanji(c, "休")) && !has(v, n, kanji(c, "上")), "休 waits for 亻, 上 for 卜");
    /* the ordering: teaching order, the lesson list is capped by max */
    CHECK(srs_lessons(&s, v, 3, &level) == n, "the count is all of them, whatever max is");
    srs_close(&s);
    /* persisted: the same after folding and reopening */
    CHECK(srs_open(&s, c, TMP, T0, 0) == 0, "reopen");
    CHECK(srs_lessons(&s, NULL, 0, &level) == n && level == 2, "the same after a reopen");
    srs_close(&s);
    course_close(c); free(c);

    /* unlock_after, in the features deck: comedor waits for comida */
    c = open_course(FEATURES);
    fresh_dir();
    CHECK(srs_open(&s, c, TMP, T0, 0) == 0, "open features");
    uint32_t comer = item_of(c, "comer"), comida = item_of(c, "comida"), suffix = item_of(c, "-ida");
    learn(&s, c, comer, 2); learn(&s, c, suffix, 2);          /* known is stage 3 here */
    n = srs_lessons(&s, v, 100, &level);
    CHECK(has(v, n, comida), "comida: its parts are known");
    learn(&s, c, comida, 0);
    n = srs_lessons(&s, v, 100, &level);
    CHECK(!has(v, n, item_of(c, "comedor")), "comedor waits for comida to be known (unlock_after)");
    srs_close(&s);
    course_close(c); free(c);
}

/* What's due, and the counts, across puts and rescans */
static void scanning(void){
    Course *c = open_course(DEMO);
    Srs s;
    fresh_dir();
    CHECK(srs_open(&s, c, TMP, T0, 0) == 0, "open");
    for(uint32_t n = 0; n < 8; n++) learn(&s, c, n, 0);         /* due at T0 + 2h */
    CHECK(s.started == 8 && s.n_due == 0 && s.next_due == T0 + 2 * H, "none due yet; next at +2h");
    CHECK(s.group_count[0] == 8, "8 Learning");
    CHECK(srs_scan(&s, T0 + 3 * H) == 0 && s.n_due == 8 && s.due_total == 8, "all 8 due at +3h");
    SrsRec r;
    srs_get(&s, 0, &r);
    srs_grade(c, &r, SRS_GOOD, T0 + 3 * H, 0);
    CHECK(srs_put(&s, 0, &r, T0 + 3 * H) == 0 && s.n_due == 7 && s.due_total == 7, "one reviewed: 7 due");
    CHECK(s.group_count[0] == 7 && s.group_count[1] == 1, "7 Learning, 1 Known");
    CHECK(srs_scan(&s, T0 + 3 * H) == 0 && s.n_due == 7 && s.group_count[1] == 1, "a rescan agrees");
    /* undo: the old record, put back */
    SrsRec before;
    srs_get(&s, 1, &before);
    r = before;
    srs_grade(c, &r, SRS_AGAIN, T0 + 3 * H, 0);
    srs_put(&s, 1, &r, T0 + 3 * H);
    srs_put(&s, 1, &before, T0 + 3 * H);
    SrsRec now;
    CHECK(srs_get(&s, 1, &now) == 1 && !memcmp(&now, &before, sizeof now), "undo");
    srs_close(&s);
    CHECK(srs_open(&s, c, TMP, T0 + 3 * H, 0) == 0 && srs_get(&s, 1, &now) == 1 && !memcmp(&now, &before, sizeof now),
          "the undo survives a fold");
    srs_close(&s);
    course_close(c); free(c);
}

/* ---- a round's queue (study.c) ---- */

static void rounds(void){
    StSession s;
    uint16_t items[3] = { 10, 20, 30 };
    uint8_t quiz[3] = { ST_MEANING | ST_READING, ST_MEANING, ST_MEANING | ST_READING };
    CHECK(st_begin(&s, items, quiz, 3, 1234) == 0 && st_left(&s) == 5, "5 questions for 3 items");
    /* all right: each item finishes once, with its last question */
    int finished[3] = { 0 }, asked[3] = { 0 };
    StQ q;
    StItem done;
    while(st_current(&s, &q)){
        int k = q.item / 10 - 1;
        asked[k] |= q.q;
        int r = st_answer(&s, SRS_GOOD, &done);
        if(r){ CHECK(done.item == q.item && done.grade == SRS_GOOD && !done.wrong, "item %u done right", q.item);
               CHECK(asked[k] == quiz[k], "item %u finished only after all its questions", q.item);
               finished[k]++; }
    }
    CHECK(finished[0] == 1 && finished[1] == 1 && finished[2] == 1 && s.items_done == 3 && s.missed == 0, "each once");
    st_end(&s);

    /* a wrong answer: the question comes back 2 to 5 on, the item's grade
     * is the worst it got */
    CHECK(st_begin(&s, items, quiz, 3, 99) == 0, "begin");
    st_current(&s, &q);
    StQ first = q;
    CHECK(st_answer(&s, SRS_AGAIN, &done) == 0 && st_left(&s) == 5 && s.missed == 1, "wrong: requeued");
    int back = -1;
    for(int i = s.pos; i < s.nq; i++) if(s.q[i].item == first.item && s.q[i].q == first.q){ back = i - s.pos; break; }
    CHECK(back >= 1 && back <= 4, "it comes back %d questions later", back + 1);
    /* undo the wrong answer: the requeue goes away, the question returns */
    uint16_t it;
    CHECK(st_undo(&s, &it) == 0 && it == first.item && st_left(&s) == 5 && s.missed == 0, "undo a wrong answer");
    CHECK(st_current(&s, &q) && q.item == first.item && q.q == first.q, "the same question again");
    CHECK(st_undo(&s, &it) == -1, "one level of undo");
    /* now answer everything, the first question wrong once */
    int n = 0, wrong_once = 0;
    while(st_current(&s, &q)){
        int g = SRS_GOOD;
        if(q.item == first.item && q.q == first.q && !wrong_once){ g = SRS_AGAIN; wrong_once = 1; }
        if(st_answer(&s, g, &done) && done.item == first.item)
            CHECK(done.wrong && done.grade == SRS_AGAIN, "the item that was missed is graded Again");
        if(++n > 20) break;
    }
    CHECK(s.items_done == 3 && s.items_wrong == 1 && s.answered == 6, "3 done, 1 wrong, 6 answers: %u %u %u",
          s.items_done, s.items_wrong, s.answered);
    st_end(&s);

    /* undo after an item finishes: the caller is told, to put its record back */
    uint16_t one[1] = { 7 };
    uint8_t m[1] = { ST_MEANING };
    st_begin(&s, one, m, 1, 5);
    CHECK(st_answer(&s, SRS_EASY, &done) == 1 && done.grade == SRS_EASY && st_left(&s) == 0, "a one-question item");
    CHECK(st_undo(&s, &it) == 1 && it == 7 && st_left(&s) == 1 && s.items_done == 0, "undo reopens it");
    st_end(&s);

    /* random rounds: whatever the answers, every item finishes exactly once,
     * after every one of its questions was answered right */
    uint16_t many[40];
    uint8_t mq[40];
    for(int i = 0; i < 40; i++){ many[i] = (uint16_t)(i + 1); mq[i] = (uint8_t)(1 + i % 3); }
    uint32_t x = 7;
    for(int seed = 1; seed <= 300; seed++){
        CHECK(st_begin(&s, many, mq, 40, (uint32_t)seed) == 0, "begin %d", seed);
        int fin[41] = { 0 }, steps = 0;
        uint8_t right[41] = { 0 };
        while(st_current(&s, &q) && steps < 2000){
            x = x * 1103515245u + 12345u;
            int g = (x >> 16) % 4 == 0 ? SRS_AGAIN : SRS_GOOD;
            if(g != SRS_AGAIN) right[q.item] |= q.q;
            if(st_answer(&s, g, &done)){
                fin[done.item]++;
                if((right[done.item] & mq[done.item - 1]) != mq[done.item - 1]) { CHECK(0, "finished early"); }
            }
            if((x >> 20) % 7 == 0 && st_undo(&s, &it) >= 0){
                /* an undo, then the same answer again */
                st_current(&s, &q);
                if(st_answer(&s, g, &done)) { /* counted above already */ fin[done.item] += 0; }
            }
            steps++;
        }
        int all = 1;
        for(int i = 1; i <= 40; i++) if(fin[i] > 1) all = 0;
        CHECK(!st_current(&s, &q) && s.items_done == 40 && all, "seed %d: every item once (%u)", seed, s.items_done);
        st_end(&s);
    }
}

/* ---- the card: installing the demo, listing courses ---- */

static void card(void){
    const char *root = "build/srs_root";
    char p[160];
    mkdir(root, 0777);
    st_remove_course(root, "demo-kanji");
    snprintf(p, sizeof p, "%s/study/.demo", root); remove(p);
    snprintf(p, sizeof p, "%s/study/last.txt", root); remove(p);
    CHECK(st_install_demo(root, 0) == 1, "the first open installs the demo");
    static uint8_t a[100000], b[100000];
    snprintf(p, sizeof p, "%s/study/demo-kanji/course.srs", root);
    long na = slurp(p, a, sizeof a), nb = slurp(DEMO, b, sizeof b);
    CHECK(na == nb && na > 0 && !memcmp(a, b, na), "the installed course is the demo, byte for byte");
    CHECK(st_install_demo(root, 0) == 0, "only once");
    char ids[8][ST_ID_MAX];
    CHECK(st_list_courses(root, ids, 8) == 1 && !strcmp(ids[0], "demo-kanji"), "one course on the card");
    CHECK(st_remove_course(root, "demo-kanji") == 0 && st_list_courses(root, ids, 8) == 0, "removed");
    CHECK(st_install_demo(root, 0) == 0, "removing it keeps it removed");
    CHECK(st_install_demo(root, 1) == 1 && st_list_courses(root, ids, 8) == 1, "reinstalled on request");
    CHECK(st_remove_course(root, "../x") == -1, "an id can't climb out of study/");
    snprintf(p, sizeof p, "%s/study/b-course", root); mkdir(p, 0777);
    snprintf(p, sizeof p, "%s/study/b-course/course.srs", root); spit(p, b, 10);
    snprintf(p, sizeof p, "%s/study/empty", root); mkdir(p, 0777);
    CHECK(st_list_courses(root, ids, 8) == 2 && !strcmp(ids[0], "b-course") && !strcmp(ids[1], "demo-kanji"),
          "sorted, and a folder without a course.srs isn't one");
    st_remove_course(root, "b-course");
    snprintf(p, sizeof p, "%s/study/empty", root); rmdir(p);
    char id[ST_ID_MAX];
    CHECK(st_last_get(root, id) == 0 && !id[0], "no last course yet");
    st_last_set(root, "demo-kanji");
    CHECK(st_last_get(root, id) == 1 && !strcmp(id, "demo-kanji"), "the last course");
}

int main(int argc, char **argv){
    if(argc == 3 && !strcmp(argv[1], "sm2trace")){
        Course *c = open_course(CARDS);
        int lines;
        uint32_t crc = sm2_year(c, atoi(argv[2]), stdout, &lines);
        fprintf(stderr, "%d reviews, crc %08x\n", lines, (unsigned)crc);
        course_close(c); free(c);
        return 0;
    }
    stages();
    sm2();
    log_truncation();
    remap();
    unlocks();
    scanning();
    rounds();
    card();
    printf("srs_test (%zu-bit): %s\n", sizeof(void *) * 8, fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
