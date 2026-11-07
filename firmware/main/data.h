/* data.h -- the Memos database (MemoDB.pdb on the SD card) for the UI.
 *
 * Memos is the only data app. Date Book, Address and To Do were archived with
 * the iCloud sync on 2026-11-07 (git tag archive/pim-caldav). The `app`
 * arguments below are kept so the list, detail and category code reads the
 * way it did; APP_MEMO is the only value. */
#ifndef DATA_H
#define DATA_H
#include <stdint.h>
#include "appinfo.h"   /* CatTable */

/* Make sure the card has a MemoDB.pdb (an empty one with the standard
 * categories if not; no demo records) and finish any interrupted rewrite.
 * Safe to call every boot. */
void data_init(void);

enum { APP_MEMO = 3 };

/* per-record display callback: record uid + primary line + optional secondary. */
typedef void (*data_row_cb)(uint32_t uid, const char *primary, const char *secondary, void *ctx);

/* absolute SD path of the database (for Find over the raw file). */
const char *data_db_path(int app);

/* every memo in the current category, as its first line. */
void data_memo(data_row_cb cb, void *ctx);

/* Memo records are plain text: read/write the whole memo string. */
int data_get_memo(uint32_t uid, char *out, int cap);        /* 1 if found */
int data_save_memo(uint32_t uid, int cat, const char *text); /* uid 0 = new */

/* the full text of one memo into out. 1 if found. */
int data_detail(int app, uint32_t uid, char *out, int cap);

/* the uid the last save that CREATED a record gave it (0 before any). A form
 * saved on the way out reopens on its record, and a new one has no uid until
 * the card has assigned it. */
uint32_t data_last_new_uid(void);
/* category index of a record (attr nibble), or -1 if not found */
int data_record_category(int app, uint32_t uid);
/* delete a memo by uid. 1 on success. */
int data_delete(int app, uint32_t uid);

/* categories: read the category table; get/set the active list filter
 * (-1 = All). The list iterator honours the filter. */
int  data_get_categories(int app, CatTable *t);
/* write the category table back (preserving records). 1 on success. */
int  data_set_categories(int app, const CatTable *t);
void data_set_category(int cat);
int  data_get_category(void);

#endif
