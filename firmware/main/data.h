/* data.h -- PDB data access for the UI (read records from SD as display rows). */
#ifndef DATA_H
#define DATA_H
#include <stdint.h>
#include "appinfo.h"   /* CatTable */

/* seed demo PDBs on the SD card for any that are missing (so views have content
 * before a HotSync). safe to call every boot. */
void data_seed_if_empty(void);

enum { APP_CAL, APP_ADDR, APP_TODO, APP_MEMO };

/* per-record display callback: record uid + primary line + optional secondary. */
typedef void (*data_row_cb)(uint32_t uid, const char *primary, const char *secondary, void *ctx);

/* iterate records of each app, decoding to display strings. */
void data_datebook(data_row_cb cb, void *ctx);
void data_address(data_row_cb cb, void *ctx);
void data_todo(data_row_cb cb, void *ctx);
void data_memo(data_row_cb cb, void *ctx);

/* Memo records are plain text: read/write the whole memo string. */
int data_get_memo(uint32_t uid, char *out, int cap);        /* 1 if found */
int data_save_memo(uint32_t uid, int cat, const char *text); /* uid 0 = new */

/* format the full detail of one record (by app + uid) into out. 1 if found. */
int data_detail(int app, uint32_t uid, char *out, int cap);

/* --- editing: load a record into a struct, save it back to the PDB --- */
#include "palm.h"
int data_get_cal(uint32_t uid, Appt *out);   /* 1 if found */
int data_get_addr(uint32_t uid, Addr *out);
int data_get_todo(uint32_t uid, Todo *out);
/* write the record back (uid==0 => create new). cat = category index, or -1 to
 * keep the record's current category. sets the Palm dirty bit. 1 on success. */
int data_save_cal(uint32_t uid, int cat, const Appt *in);
int data_save_addr(uint32_t uid, int cat, const Addr *in);
int data_save_todo(uint32_t uid, int cat, const Todo *in);
/* category index of a record (attr nibble), or -1 if not found */
int data_record_category(int app, uint32_t uid);
/* delete a record by uid (next sync propagates it). */
int data_delete(int app, uint32_t uid);

/* categories: read the app's category table; get/set the active list filter
 * (-1 = All). List iterators honor the filter. */
int  data_get_categories(int app, CatTable *t);
void data_set_category(int cat);
int  data_get_category(void);

#endif
