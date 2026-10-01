/* course_test.c -- host gate for the Study course reader (firmware/main/course.c).
 *
 *   course_test            the checks below, on the committed course files
 *   course_test dump FILE  the canonical dump of FILE, which must be identical,
 *                          byte for byte, to `tools/mkcourse.py --dump FILE`:
 *                          the round trip (make -C sim course runs both)
 *
 * Run from sim/ (the paths below are relative to it). Built 64-bit by `course`
 * and 32-bit by `smoke32`, since the device is 32-bit. */
#include "course.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO     "../courses/demo-kanji/course.srs"
#define CARDS    "../tests/data/study/cards/course.srs"
#define FEATURES "../tests/data/study/features/course.srs"

static int fails;
#define CHECK(cond, ...) do { if(!(cond)){ fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while(0)

/* ---- the dump: keep it in step with dump_file() in tools/mkcourse.py ---- */

static void esc(const char *s, uint32_t n){
    putchar('"');
    for(uint32_t i = 0; i < n; i++){
        unsigned char ch = (unsigned char)s[i];
        if(ch == '"' || ch == '\\') printf("\\%c", ch);
        else if(ch < 0x20) printf("\\x%02x", ch);
        else putchar(ch);
    }
    putchar('"');
}

static uint32_t le32(const uint8_t *p){ return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int dump_fields(Course *c, const uint8_t *t, uint32_t len){
    uint32_t pos = 0;
    CourseField f;
    while(course_field_next(t, len, &pos, &f) == 1){
        printf("  field %02x attr=%02x", f.tag, f.attr);
        if(f.attr & CF_A_PICTURE){
            CoursePic p;
            if(course_pic(c, f.pic, &p)) return 1;
            printf(" pic=%ux%u/%u crc=%08x", p.w, p.h, p.bpp, (unsigned)p.crc);
        }
        putchar(' ');
        esc(f.text, f.len);
        putchar('\n');
    }
    return 0;
}

static int dump(const char *path){
    static Course c;
    static uint8_t buf[COURSE_VERIFY_BUF];
    if(course_open(&c, path)){ fprintf(stderr, "course_test: %s: %s\n", path, c.err); return 1; }
    if(course_verify(&c, buf, sizeof buf)){ fprintf(stderr, "course_test: %s: %s\n", path, c.err); return 1; }
    printf("course items=%u levels=%u fingerprint=%08x\n", (unsigned)c.n_items, (unsigned)c.n_levels,
           (unsigned)c.fingerprint);
    uint16_t k, n;
    for(uint32_t i = 0; course_meta_entry(&c, i, &k, buf, sizeof buf, &n) == 0; i++){
        printf("meta %04x ", k);
        if((k >= 0x01 && k <= 0x08) || k == 0x11) esc((const char *)buf, n);
        else if(k == 0x10){ printf("kind quiz=%u flags=%u ", buf[0], buf[1]); esc((const char *)buf + 2, n - 2); }
        else if(k == 0x12){ printf("group from=%u ", (unsigned)le32(buf)); esc((const char *)buf + 4, n - 4); }
        else {
            printf("nums");
            for(uint16_t j = 0; j + 4 <= n; j += 4){
                if(k == 0x43) printf(" %d", (int)(int32_t)le32(buf + j));
                else printf(" %u", (unsigned)le32(buf + j));
            }
        }
        putchar('\n');
    }
    uint32_t item = 0;
    for(uint32_t i = 0; i < c.n_levels; i++){
        CourseLevel l;
        int r;
        if(course_level(&c, i, &l) || (r = course_level_load(&c, &l, buf, sizeof buf)) < 0) return 1;
        printf("level %u part=%u/%u first=%u count=%u\n", l.number, l.part, l.parts, (unsigned)l.first, (unsigned)l.count);
        if(dump_fields(&c, buf, (uint32_t)r)) return 1;
        for(uint32_t j = 0; j < l.count; j++, item++){
            CourseItem it;
            if(course_item(&c, item, &it) || course_item_load(&c, &it, buf, sizeof buf) < 0) return 1;
            printf("item %u id=%u level=%u kind=%u rank=%u\n", (unsigned)item, (unsigned)it.id, it.level, it.kind, it.rank);
            /* the pictures' headers are read into the same buffer's tail */
            if(dump_fields(&c, buf, it.text_len)) return 1;
            for(uint16_t q = 0; q < it.n_links; q++){
                CourseLink lk;
                course_link(buf, &it, q, &lk);
                printf("  link %u -> %u role=%u\n", lk.type, (unsigned)lk.target, lk.role);
            }
        }
    }
    course_close(&c);
    return 0;
}

/* ---- checks ---- */

static uint8_t *slurp(const char *path, long *n){
    FILE *f = fopen(path, "rb");
    if(!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if(fread(b, 1, *n, f) != (size_t)*n){ free(b); b = NULL; }
    fclose(f);
    return b;
}

static int open_bytes(Course *c, uint8_t *b, long n){
    FILE *f = fmemopen(b, n, "rb");
    return f ? course_open_fp(c, f) : COURSE_EIO;
}

/* the text of field `tag` number `which` (0 = the first) of item n */
static int field_text(Course *c, uint32_t n, uint8_t tag, int which, char *out, uint32_t cap, uint8_t *attr){
    static uint8_t buf[COURSE_ITEM_BUF];
    CourseItem it;
    if(course_item(c, n, &it) || course_item_load(c, &it, buf, sizeof buf) < 0) return -1;
    uint32_t pos = 0;
    CourseField f;
    while(course_field_next(buf, it.text_len, &pos, &f) == 1) if(f.tag == tag && !which--){
        uint32_t k = f.len < cap - 1 ? f.len : cap - 1;
        memcpy(out, f.text, k); out[k] = 0;
        if(attr) *attr = f.attr;
        return 0;
    }
    return -1;
}

static uint32_t find_term(Course *c, const char *term){
    char t[64];
    for(uint32_t n = 0; n < c->n_items; n++)
        if(!field_text(c, n, CF_TERM, 0, t, sizeof t, NULL) && !strcmp(t, term)) return n;
    return 0xFFFFFFFF;
}

static void check_demo(void){
    static Course c;
    static uint8_t buf[COURSE_VERIFY_BUF];
    char t[256];
    uint8_t attr;
    int r = course_open(&c, DEMO);
    CHECK(r == 0, "open the demo: %s", c.err);
    if(r) return;
    CHECK(course_verify(&c, buf, sizeof buf) == 0, "verify the demo: %s", c.err);
    CHECK(!strcmp(c.id, "demo-kanji"), "id %s", c.id);
    CHECK(c.n_items == 78 && c.n_levels == 2, "%u items, %u levels", (unsigned)c.n_items, (unsigned)c.n_levels);
    CHECK(c.n_kinds == 3 && !strcmp(c.kinds[1].name, "kanji") && c.kinds[1].quiz == 3 && c.kinds[1].flags == 1,
          "kinds");
    CHECK(c.scheduler == CS_STAGES && c.n_steps == 10 && c.steps[0] == 2 * 3600 && c.steps[1] == 4 * 3600 && c.steps[9] == 0,
          "stages: %u, first %u", c.n_steps, (unsigned)c.steps[0]);
    CHECK(c.known == 2 && c.drop == 1 && c.drop_high == 2 && c.high_from == 6, "known/drops");
    CHECK(c.grading == 2 && c.batch == 5 && c.level_pct == 90 && c.by_links == 1, "grading/batch/unlock");
    CHECK(c.n_groups == 5 && c.groups[1].from == 2 && !strcmp(c.groups[1].name, "Known") && c.groups[4].from == 10,
          "groups");
    CHECK(course_meta_text(&c, 0x0004, t, sizeof t) > 0 && !strcmp(t, "The CYD Palm project"), "author '%s'", t);
    CHECK(course_meta_text(&c, 0x0004, t, 8) == 7 && !strcmp(t, "The CYD"), "author cut to fit '%s'", t);
    CHECK(course_meta_text(&c, 0x0099, t, sizeof t) == 0 && !t[0], "an absent key");

    /* 明 is built from the radicals 日 and 月; its primary reading is めい */
    uint32_t n = find_term(&c, "明");
    CHECK(n != 0xFFFFFFFF, "find 明");
    if(n != 0xFFFFFFFF){
        CourseItem it;
        CHECK(course_item(&c, n, &it) == 0 && c.kinds[it.kind].name[0] == 'k', "明 is a kanji");
        CHECK(course_item_load(&c, &it, buf, sizeof buf) > 0, "load 明");
        CHECK(it.n_links == 2, "明 has %u links", it.n_links);
        for(uint16_t i = 0; i < it.n_links && i < 2; i++){
            CourseLink l;
            course_link(buf, &it, i, &l);
            CHECK(l.type == CL_BUILT_FROM, "a built_from link");
            char term[16];
            CHECK(field_text(&c, l.target, CF_TERM, 0, term, sizeof term, NULL) == 0
                  && !strcmp(term, i ? "月" : "日"), "明 link %u -> %s", i, term);
        }
        CHECK(field_text(&c, n, CF_READING, 0, t, sizeof t, &attr) == 0 && !strcmp(t, "めい")
              && (attr & CF_A_PRIMARY) && (attr & CF_A_KANA) && (attr & CF_A_VALUE) == 1, "明's reading %s %02x", t, attr);
        CHECK(field_text(&c, n, CF_TERM, 0, t, sizeof t, &attr) == 0 && (attr & CF_A_PICTURE), "明 is drawn");
        CHECK(field_text(&c, n, CF_MEANING, 0, t, sizeof t, &attr) == 0 && !strcmp(t, "bright")
              && attr == CF_A_PRIMARY, "明 means %s", t);
        CHECK(it.rank == 67, "明's rank %u", it.rank);
    }
    /* the 64 px prompt picture: its rows, in two strips, match its CRC */
    if(n != 0xFFFFFFFF){
        CourseItem it;
        course_item(&c, n, &it);
        course_item_load(&c, &it, buf, sizeof buf);
        uint32_t pos = 0;
        CourseField f;
        course_field_next(buf, it.text_len, &pos, &f);
        CoursePic p;
        CHECK(course_pic(&c, f.pic, &p) == 0 && p.bpp == 1 && p.w > 40 && p.w <= 64 && p.h > 40 && p.h <= 70,
              "明's picture %ux%u", p.w, p.h);
        uint8_t *rows = malloc((size_t)p.h * p.stride);
        uint16_t half = p.h / 2;
        CHECK(course_pic_rows(&c, &p, 0, half, rows, (uint32_t)half * p.stride) == 0
              && course_pic_rows(&c, &p, half, p.h - half, rows + (size_t)half * p.stride, (uint32_t)(p.h - half) * p.stride) == 0,
              "read the rows");
        CHECK(course_crc32(0, rows, (uint32_t)p.h * p.stride) == p.crc, "the rows' CRC");
        CHECK(course_pic_rows(&c, &p, 0, p.h, rows, (uint32_t)p.h * p.stride - 1) == COURSE_ERANGE, "too small a buffer");
        CHECK(course_pic_rows(&c, &p, p.h, 1, rows, (uint32_t)p.h * p.stride) == COURSE_ERANGE, "past the last row");
        free(rows);
    }
    /* every id is found, and the index walks in id order */
    uint32_t prev = 0;
    for(uint32_t j = 0; j < c.n_items; j++){
        uint32_t id, k, m;
        CHECK(course_idx(&c, j, &id, &k) == 0 && id > prev, "idx %u", (unsigned)j);
        CHECK(course_find(&c, id, &m) == 0 && m == k, "find id %u", (unsigned)id);
        prev = id;
    }
    uint32_t m;
    CHECK(course_find(&c, 0, &m) == COURSE_ERANGE && course_find(&c, 999999, &m) == COURSE_ERANGE, "missing ids");
    CourseItem it;
    CHECK(course_item(&c, c.n_items, &it) == COURSE_ERANGE, "item past the end");
    CHECK(course_item(&c, 0, &it) == 0 && course_item_load(&c, &it, buf, it.text_len - 1) == COURSE_ERANGE,
          "a buffer too small for the item");
    CHECK(course_verify(&c, buf, COURSE_VERIFY_BUF - 1) == COURSE_ERANGE, "verify needs its scratch");
    /* levels: 1 then 2, covering 40 + 38 items */
    CourseLevel l;
    CHECK(course_level(&c, 0, &l) == 0 && l.number == 1 && l.first == 0 && l.count == 40, "level 1");
    CHECK(course_level(&c, 1, &l) == 0 && l.number == 2 && l.first == 40 && l.count == 38, "level 2");
    course_close(&c);
}

static void check_decks(void){
    static Course c;
    static uint8_t buf[COURSE_VERIFY_BUF];
    char t[128];
    uint8_t attr;
    CHECK(course_open(&c, CARDS) == 0, "open cards: %s", c.err);
    CHECK(course_verify(&c, buf, sizeof buf) == 0, "verify cards: %s", c.err);
    CHECK(c.scheduler == CS_SM2 && c.n_steps == 5 && c.steps[4] == 16 * 86400, "sm2 ladder");
    CHECK(c.mult[0] == 60 && c.mult[1] == 100 && c.mult[2] == 160, "multipliers");
    CHECK(c.ease[0] == 250 && c.ease[1] == 130, "ease");
    CHECK(c.ease_change[0] == -20 && c.ease_change[1] == -15 && c.ease_change[2] == 0 && c.ease_change[3] == 15,
          "ease changes (the defaults)");
    CHECK(c.relearn == 600 && c.day_aligned == 1 && c.known == 1 && c.grading == 4, "relearn/day/known/grading");
    CHECK(c.n_kinds == 1 && !strcmp(c.kinds[0].name, "card") && c.n_levels == 1, "one kind, one level");
    uint32_t n = find_term(&c, "Österreich");
    CHECK(n != 0xFFFFFFFF && field_text(&c, n, CF_MEANING, 0, t, sizeof t, &attr) == 0 && !strcmp(t, "Wien")
          && !(attr & (CF_A_PICTURE | CF_A_KANA)), "Latin-1 as Palm text");
    n = find_term(&c, "Côte d'Ivoire");
    CHECK(n != 0xFFFFFFFF, "the curly apostrophe was folded");
    n = find_term(&c, "日本");
    CHECK(n != 0xFFFFFFFF && field_text(&c, n, CF_MEANING, 0, t, sizeof t, &attr) == 0 && (attr & CF_A_PICTURE),
          "a picture meaning");
    course_close(&c);

    CHECK(course_open(&c, FEATURES) == 0, "open features: %s", c.err);
    CHECK(course_verify(&c, buf, sizeof buf) == 0, "verify features: %s", c.err);
    CHECK(c.n_roles == 2 && !strcmp(c.roles[1], "suffix"), "roles");
    CHECK(c.n_levels == 3, "levels");
    CourseLevel l;
    CHECK(course_level(&c, 1, &l) == 0 && l.part == 2 && l.parts == 2, "part 2 of 2");
    CHECK(course_level(&c, 2, &l) == 0 && l.number == 7, "level 7");
    n = find_term(&c, "comer");
    CHECK(n != 0xFFFFFFFF && field_text(&c, n, CF_SENSE, 1, t, sizeof t, &attr) == 0
          && !strcmp(t, "verb\x1fto have lunch\x1fregional,intransitive") && (attr & CF_A_VALUE) == 2, "a sense");
    CHECK(field_text(&c, n, CF_EXAMPLE, 0, t, sizeof t, &attr) == 0 && (attr & CF_A_VALUE) == 2, "an example's sense");
    CHECK(field_text(&c, n, CF_ETYMOLOGY, 0, t, sizeof t, &attr) == 0 && !(attr & CF_A_PICTURE)
          && strstr(t, "compare Greek (phagein") != NULL, "the etymology was stripped: %s", t);
    CHECK(field_text(&c, n, CF_FORM, 1, t, sizeof t, NULL) == 0 && !strcmp(t, "past participle\x1f" "comido"), "a form");
    n = find_term(&c, "comida");
    if(n != 0xFFFFFFFF){
        CourseItem it;
        course_item(&c, n, &it);
        course_item_load(&c, &it, buf, sizeof buf);
        CourseLink a, b, f;
        course_link(buf, &it, 0, &a);
        course_link(buf, &it, 1, &b);
        course_link(buf, &it, 2, &f);
        CHECK(it.n_links == 3 && a.type == CL_BUILT_FROM && a.role == 1 && b.role == 2 && f.type == CL_FAMILY,
              "comida's links");
    }
    n = find_term(&c, "つき");
    CHECK(n != 0xFFFFFFFF && field_text(&c, n, CF_TERM, 0, t, sizeof t, &attr) == 0 && (attr & CF_A_KANA)
          && !(attr & CF_A_PICTURE), "a kana term");
    CHECK(field_text(&c, n, CF_READING, 3, t, sizeof t, &attr) == 0 && (attr & CF_A_VALUE) == 3, "a nanori");
    /* the builder's folding: typography to ASCII, Latin letters outside
     * Latin-1 to their base letter (not in TERM or READING), lines joined */
    n = find_term(&c, "fold");
    CHECK(n != 0xFFFFFFFF && field_text(&c, n, CF_MEANING, 0, t, sizeof t, &attr) == 0
          && !strcmp(t, "Dvor\xc3\xa1k's CO2 -> H2O") && !(attr & CF_A_PICTURE), "folded meaning: %s", t);
    CHECK(field_text(&c, n, CF_EXAMPLE, 0, t, sizeof t, &attr) == 0 && !strcmp(t, "Kept, with Capek.")
          && !(attr & CF_A_PICTURE), "Latin fallback in an example: %s", t);
    CHECK(field_text(&c, n, CF_ETYMOLOGY, 0, t, sizeof t, NULL) == 0
          && !strcmp(t, "First line. Second line, with s and bh."), "lines joined: %s", t);
    n = find_term(&c, "drop");
    CHECK(n != 0xFFFFFFFF && field_text(&c, n, CF_ETYMOLOGY, 0, t, sizeof t, NULL) == -1,
          "a stripped field with nothing readable left is dropped");
    course_close(&c);
}

static void check_damage(void){
    static Course c;
    long n;
    uint8_t *b = slurp(FEATURES, &n);
    CHECK(b != NULL, "read features");
    if(!b) return;
    CHECK(open_bytes(&c, b, n) == 0, "from memory: %s", c.err);
    course_close(&c);
    CHECK(open_bytes(&c, b, n - 1) == COURSE_EBAD && strstr(c.err, "in part"), "truncated: %s", c.err);
    b[0] = 'X';
    CHECK(open_bytes(&c, b, n) == COURSE_EBAD && strstr(c.err, "not a course"), "magic: %s", c.err);
    b[0] = 'S';
    b[4] = 2;                                   /* major version 2 */
    CHECK(open_bytes(&c, b, n) == COURSE_EVERSION && strstr(c.err, "newer Study"), "version: %s", c.err);
    b[4] = 1;
    b[40] ^= 1;                                 /* inside the section table */
    CHECK(open_bytes(&c, b, n) == COURSE_EBAD, "table: %s", c.err);
    b[40] ^= 1;
    CHECK(open_bytes(&c, b, n) == 0, "restored");
    course_close(&c);
    CHECK(course_open(&c, "/nonexistent/course.srs") == COURSE_EIO, "no file");
    free(b);
}

int main(int argc, char **argv){
    if(argc == 3 && !strcmp(argv[1], "dump")) return dump(argv[2]);
    CHECK(course_crc32(0, "123456789", 9) == 0xCBF43926, "the CRC-32 check value");
    CHECK(course_crc32(course_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926, "a CRC in two parts");
    check_demo();
    check_decks();
    check_damage();
    printf("course_test (%zu-bit): %s\n", sizeof(void *) * 8, fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
