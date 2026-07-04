/* data.h -- PDB data access for the UI (read records from SD as display rows). */
#ifndef DATA_H
#define DATA_H

/* seed demo PDBs on the SD card for any that are missing (so views have content
 * before a HotSync). safe to call every boot. */
void data_seed_if_empty(void);

/* per-record display callback: primary line (required) + optional secondary. */
typedef void (*data_row_cb)(const char *primary, const char *secondary, void *ctx);

/* iterate records of each app, decoding to display strings. */
void data_datebook(data_row_cb cb, void *ctx);
void data_address(data_row_cb cb, void *ctx);
void data_todo(data_row_cb cb, void *ctx);

#endif
