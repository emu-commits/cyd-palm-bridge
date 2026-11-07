/* gurupool.h -- load the Guru habit pool off the SD card.
 *
 * The impure half of the pool: guru.c holds the logic and the built-in table,
 * this file reads /sdcard/guru.txt and installs it over the top. Same split as
 * appcfg.c over config.h, and for the same reason -- the logic layer stays
 * host-testable and free of stdio.
 *
 * The grammar is firmware/main/guru_pool.txt, which is both the source the
 * built-in table is generated from and the file the user edits. One format,
 * documented in one place, parsed by the generator and by this file.
 *
 * NOTHING HERE CAN BREAK GURU. A missing file, an unreadable card, a truncated
 * line, a duplicate id, an absurd size: every one of them leaves the built-in
 * pool installed and the app working. The card is user-editable, which means it
 * WILL eventually be malformed, so a bad pool is an expected input rather than
 * an error case.
 */
#ifndef GURUPOOL_H
#define GURUPOOL_H

#define GURUPOOL_PATH "/sdcard/guru.txt"

/* Parse `path` and install it as the active pool. Returns the number of habits
 * loaded, or -1 if the file was missing, unreadable, or rejected -- in which
 * case the previously active pool (normally the built-in one) is untouched.
 *
 * The parsed text is held in a single arena owned by this module for the life
 * of the process; guru.c borrows into it. Calling this again frees the previous
 * arena and replaces it, so a reload is safe, but any GuruTask pointer held
 * across a reload is stale. The UI rebuilds its list from guru_task() after a
 * load, so it never holds one. */
int gurupool_load(const char *path);

/* Why the last gurupool_load() returned -1, as one line for the About screen /
 * log -- "guru.txt: line 12: unknown category 'cardio'". Empty string if the
 * last load succeeded or none has run. Never NULL. */
const char *gurupool_error(void);

/* 1 if the active pool came from the card rather than the firmware. */
int gurupool_from_sd(void);

/* Write the BUILT-IN pool to `path` in the editable format, so the user has a
 * correct file to start from rather than a blank page. Returns 0, or -1 if the
 * card could not be written. Refuses to overwrite an existing file -- the whole
 * point of the export is to seed an edit, and silently replacing a pool
 * somebody has spent time on would be the worst possible behaviour. */
int gurupool_export(const char *path);

/* ---- editing on the device ------------------------------------------------
 * Guru's own edit screen changes the list. Each call writes the WHOLE active
 * pool, with the one change, to `path` (replaced whole, through safefile.h),
 * then loads it back, so what Guru shows is always what the card holds. If
 * the card already had a guru.txt that could not be used, it is kept beside
 * the new one as guru.txt.bak rather than written over.
 *
 * Text is cleaned on the way in: a '|' becomes '/', a line break or tab a
 * space, and the name is cut to its 30 characters. Both name and why must be
 * non-empty after that (the file format requires both).
 *
 * gurupool_put: id 0 adds a habit under the lowest free id (1..64); any other
 * id replaces that habit. Returns the habit's id, or -1 (gurupool_error()
 * says why: no free id, an empty field, the card).
 * gurupool_remove: drops habit `id`. Refuses to remove the last one, since an
 * empty list is not a list. 0, or -1. */
int gurupool_put(const char *path, int id, int cat, const char *name, const char *why);
int gurupool_remove(const char *path, int id);

#endif
