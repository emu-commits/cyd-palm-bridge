/* course.h -- read a Study course file (course.srs), a piece at a time.
 *
 * The format is docs/COURSE_FORMAT.md; tools/mkcourse.py writes it. This reader
 * is pure C and stdio: no LVGL, no ESP-IDF, no statics, so it runs the same on
 * the device, in the simulator and in the host gates (make -C sim course; the
 * fuzz gate in make ftest).
 *
 * THE FILE IS UNTRUSTED. Every offset and length is checked before it's used,
 * and a damaged or hostile file gets an error and a message in c->err, never a
 * crash or a read outside a buffer. The cheap checks happen on open; each item
 * and picture carries its own CRC, checked when it's read; course_verify()
 * checks everything (COURSE_FORMAT.md section 6).
 *
 * MEMORY: a Course is about 1.3 KB and lives wherever the caller puts it (the
 * Study session allocates it on the heap). Nothing here allocates except
 * course_open(), briefly, for META (up to 4 KB, freed before it returns). One
 * item's text and links need COURSE_ITEM_BUF bytes, the caller's.
 */
#ifndef COURSE_H
#define COURSE_H

#include <stdint.h>
#include <stdio.h>

#define COURSE_FORMAT_MAJOR 1
#define COURSE_MAX_KINDS    16
#define COURSE_MAX_ROLES    15
#define COURSE_MAX_GROUPS   15
#define COURSE_MAX_STEPS    16
#define COURSE_MAX_ITEMS    65535u
#define COURSE_TEXT_MAX     4096
#define COURSE_LINKS_MAX    64
#define COURSE_META_MAX     4096
#define COURSE_ITEM_BUF     (COURSE_TEXT_MAX + 8 * COURSE_LINKS_MAX)
#define COURSE_VERIFY_BUF   (COURSE_ITEM_BUF + 256)   /* course_verify() scratch */
#define COURSE_PIC_MAX_W    240
#define COURSE_PIC_MAX_H    1024

/* results: 0 is success; these are all < 0 */
#define COURSE_EIO      (-1)    /* the card couldn't be read */
#define COURSE_EBAD     (-2)    /* the file is damaged or not a course */
#define COURSE_EVERSION (-3)    /* a course for a newer Study */
#define COURSE_ENOMEM   (-4)
#define COURSE_ERANGE   (-5)    /* the caller asked for something out of range */

/* field tags (COURSE_FORMAT.md 3.5) */
enum {
    CF_TERM = 0x01, CF_MEANING, CF_READING, CF_SENSE, CF_EXAMPLE, CF_TRANSLATION,
    CF_MNEMONIC_MEANING, CF_MNEMONIC_READING, CF_ETYMOLOGY, CF_NOTE, CF_FORM,
    CF_LEVEL_TITLE = 0x20, CF_LEVEL_THEME, CF_LEVEL_GLOSS,
};
#define CF_A_PICTURE 0x80
#define CF_A_PRIMARY 0x40
#define CF_A_KANA    0x20
#define CF_A_VALUE   0x1F       /* reading type, or sense number */

enum { CL_UNLOCK_AFTER = 1, CL_BUILT_FROM, CL_FAMILY, CL_RELATED };
enum { CS_STAGES = 1, CS_SM2 = 2 };

typedef struct { uint32_t off, len, crc; } CourseSect;
typedef struct { char name[24]; uint8_t quiz, flags; } CourseKind;   /* quiz: 1 meaning, 2 reading */
typedef struct { uint32_t from; char name[16]; } CourseGroup;

typedef struct {
    FILE    *f;
    uint32_t size, n_items, n_levels, fingerprint;
    uint16_t minor;
    CourseSect meta, levl, item, text, bmp, link, idx;

    /* META, parsed on open. Longer texts (author, description, source) are
     * fetched when shown: course_meta_text(). */
    char     id[32], title[64], version[16], licence[32], language[16];
    uint8_t  n_kinds, n_roles, n_groups;
    CourseKind  kinds[COURSE_MAX_KINDS];
    char        roles[COURSE_MAX_ROLES][16];
    CourseGroup groups[COURSE_MAX_GROUPS];
    uint8_t  scheduler, grading, batch, level_pct, by_links, known;
    uint8_t  n_steps;                   /* stage intervals, or the sm2 ladder */
    uint32_t steps[COURSE_MAX_STEPS];   /* seconds; a stage of 0 = retired */
    uint8_t  drop, drop_high, high_from;
    uint16_t mult[3];                   /* hundredths: hard, good, easy */
    uint16_t ease[2];                   /* hundredths: start, min */
    int16_t  ease_change[4];            /* hundredths: again, hard, good, easy */
    uint32_t relearn;                   /* seconds */
    uint8_t  day_aligned;
    uint16_t render[4];                 /* wrap width; prompt, word, text heights */

    char     err[80];
} Course;

typedef struct {
    uint32_t id;
    uint16_t level;
    uint8_t  kind, flags;
    uint32_t text_off;
    uint16_t text_len, n_links;
    uint32_t link_off;
    uint16_t rank;
    uint32_t crc;
    uint32_t n;                         /* its item number */
} CourseItem;

typedef struct {
    uint16_t number;
    uint8_t  part, parts;
    uint32_t first, count;
    uint32_t text_off;
    uint16_t text_len;
    uint32_t crc;
} CourseLevel;

typedef struct {
    uint8_t     tag, attr;
    uint32_t    pic;                    /* offset in BMP, when attr & CF_A_PICTURE */
    const char *text;                   /* NOT NUL-terminated: use len */
    uint16_t    len;
} CourseField;

typedef struct { uint32_t target; uint8_t type, role; } CourseLink;

typedef struct {
    uint32_t off;                       /* in BMP */
    uint16_t w, h, stride;
    uint8_t  bpp;
    uint32_t crc;
} CoursePic;

/* Open and check the cheap parts (section 6). On failure c->err says why and
 * the file is closed. */
int  course_open(Course *c, const char *path);
/* The same on a stream already open for reading (the fuzz gate's fmemopen);
 * the Course owns it from here, and course_close() closes it. */
int  course_open_fp(Course *c, FILE *f);
void course_close(Course *c);

/* An item's 32-byte entry, checked against the sections. */
int  course_item(Course *c, uint32_t n, CourseItem *it);
/* Its text and then its links, read into buf (COURSE_ITEM_BUF is always
 * enough), and checked: the CRC, every field's bounds and rules, every link.
 * Returns the bytes read. */
int  course_item_load(Course *c, const CourseItem *it, uint8_t *buf, uint32_t cap);
/* Walk the fields of loaded text: 1 = *f filled, 0 = the end. The text must
 * have come through course_item_load or course_level_load. */
int  course_field_next(const uint8_t *text, uint32_t len, uint32_t *pos, CourseField *f);
/* Link i of a loaded item: they follow its text in the same buffer. */
void course_link(const uint8_t *buf, const CourseItem *it, uint16_t i, CourseLink *l);

int  course_level(Course *c, uint32_t i, CourseLevel *l);
int  course_level_load(Course *c, const CourseLevel *l, uint8_t *buf, uint32_t cap);

/* Item number of an id (binary search of IDX), or COURSE_ERANGE if none. */
int  course_find(Course *c, uint32_t id, uint32_t *n);
/* IDX entry j (sorted by id), for walking progress and IDX side by side. */
int  course_idx(Course *c, uint32_t j, uint32_t *id, uint32_t *n);

/* A picture's header, checked; then rows y0..y0+rows-1 into buf. The CRC of
 * the rows can only be checked when all of them are read: course_pic_check. */
int  course_pic(Course *c, uint32_t off, CoursePic *p);
int  course_pic_rows(Course *c, const CoursePic *p, uint16_t y0, uint16_t rows, uint8_t *buf, uint32_t cap);
int  course_pic_check(Course *c, const CoursePic *p, uint8_t *scratch, uint32_t cap);

/* A META text value (author 0x0004, description 0x0007, source 0x0008, ...)
 * into buf, NUL-terminated and cut to fit. Returns its length, or 0 if absent. */
int  course_meta_text(Course *c, uint16_t key, char *buf, uint32_t cap);
/* Raw META entry i (for the dump): key, length, value copied into buf. */
int  course_meta_entry(Course *c, uint32_t i, uint16_t *key, uint8_t *buf, uint32_t cap, uint16_t *len);

/* Everything: every section's CRC, every level, item, link and picture, the
 * padding, and that ITEM and IDX agree. scratch needs COURSE_VERIFY_BUF bytes. */
int  course_verify(Course *c, uint8_t *scratch, uint32_t cap);

uint32_t course_crc32(uint32_t crc, const void *buf, uint32_t len);

#endif
