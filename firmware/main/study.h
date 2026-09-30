/* study.h -- one round of Study: the questions for a review session or a
 * lesson's quiz, in the order they're asked. Pure logic like srs.c, tested on
 * the host (make -C sim srs); the screens in ui.c only show what this says.
 *
 * An item with a meaning and a reading is ONE review with two questions
 * (SRS_PLAN.md section 2): each question is asked separately, in a shuffled
 * queue, and the item is graded once both have been answered right. A wrong
 * answer marks the item and puts that question back a few cards later, so it
 * comes round again in the same session. Its grade is the worst it got.
 * The last answer can be undone.
 */
#ifndef STUDY_H
#define STUDY_H

#include <stdint.h>

#define ST_MEANING 1            /* question bits, as a course's kind "quiz" */
#define ST_READING 2

typedef struct { uint16_t item; uint8_t q; uint8_t pad; } StQ;

typedef struct {
    uint16_t item;              /* the course's item number */
    uint8_t  todo;              /* questions still to answer right */
    uint8_t  grade;             /* the worst grade so far (SRS_AGAIN..SRS_EASY) */
    uint8_t  wrong;             /* answered wrong at least once */
} StItem;

typedef struct {
    StQ     *q;                 /* the queue; q[pos] is the question on screen */
    uint16_t nq, pos, cap;
    StItem  *it;
    uint16_t nit;
    uint32_t seed;
    uint16_t answered, missed;  /* questions answered, and answered wrong */
    uint16_t items_done, items_wrong;
    /* one level of undo: what the last answer changed */
    uint8_t  can_undo, undo_completed;
    uint16_t undo_pos, undo_ins;        /* undo_ins: where a requeue went, or 0xFFFF */
    uint16_t undo_idx;
    StItem   undo_item;
} StSession;

/* Start a round: `items` (n of them) with their question bits, shuffled by
 * `seed`. Returns 0, or -1 if out of memory. */
int  st_begin(StSession *s, const uint16_t *items, const uint8_t *quiz, int n, uint32_t seed);
void st_end(StSession *s);

/* The question on screen: 1 = *q set, 0 = the round is over. */
int  st_current(const StSession *s, StQ *q);
/* How many questions are left, this one included. */
int  st_left(const StSession *s);

/* Answer the question on screen with `grade` (SRS_AGAIN is wrong). Returns 1
 * if that finished its item, with the item and its worst grade in *done;
 * 0 if not. */
int  st_answer(StSession *s, int grade, StItem *done);

/* Take the last answer back. Returns 1 if it had finished an item (whose
 * record the caller must put back as it was), with that item in *item;
 * 0 if not; -1 if there's nothing to undo. */
int  st_undo(StSession *s, uint16_t *item);

/* ---- the card: /sdcard/study/<course-id>/course.srs (SRS_PLAN.md section 3) ---- */

#define ST_ID_MAX 32                    /* a course id, NUL included */

/* Write the built-in demo course to <root>/study/demo-kanji/ the first time
 * (then <root>/study/.demo marks it done, so removing the demo keeps it
 * removed), or whenever `force` is set (the "Reinstall demo" menu item).
 * Returns 1 if it wrote the course, 0 if it didn't need to, < 0 on error. */
int  st_install_demo(const char *root, int force);

/* Remove a course's folder: its course.srs and its progress. */
int  st_remove_course(const char *root, const char *id);

/* The courses on the card, sorted by id: folders under <root>/study that hold
 * a course.srs. Returns how many (at most max). */
int  st_list_courses(const char *root, char ids[][ST_ID_MAX], int max);

/* The course opened last, kept in <root>/study/last.txt. */
int  st_last_get(const char *root, char id[ST_ID_MAX]);
void st_last_set(const char *root, const char *id);

#endif
