/* objfile.h -- read and merge a server object a line at a time, off the card.
 *
 * A server object can be far bigger than anything the device can hold: a
 * contact with a photo is tens of kilobytes, and the device's fetch buffer is
 * 8 KB. Nothing here holds the object in RAM. It is read from a file (a range
 * of one, so the batched-download cache can be read in place), one physical
 * line at a time, and:
 *
 *   obj_scan    finds the master component (a recurring series is one object
 *               holding the series and its changed occurrences; the master is
 *               the VEVENT without a RECURRENCE-ID) and counts what the merge
 *               needs to know;
 *   obj_filter  writes the Palm view: the master's lines that the Palm codec
 *               reads, unfolded, and nothing else (no PHOTO, no overrides, no
 *               VTIMEZONE). That is what ical_parse / vcard_parse /
 *               vtodo_parse are given, so a big object parses in a small
 *               buffer and an override can't overwrite the series' fields;
 *   obj_merge   writes the server's object back out, byte for byte, with only
 *               the Palm-owned properties that the device changed swapped for
 *               the device's. Everything the Palm can't hold -- location,
 *               attendees, URLs, extra alarms, overrides, photos, birthdays,
 *               the sixth phone number -- is kept exactly as it was.
 *
 * Which properties the Palm owns, and how they group (DTSTART, DTEND and
 * DURATION change together), is the table in objfile.c. A group is replaced
 * only if the device's emitted lines for it differ from the server's emitted
 * lines for it, both made by the same emitter, so an untouched field is never
 * rewritten. */
#ifndef OBJFILE_H
#define OBJFILE_H
#include <stdio.h>

typedef struct {
    int  ok;           /* a master component was found */
    int  master;       /* ordinal of the master among components of its type */
    int  nMasterAdr;   /* ADR lines in the master (the Palm keeps the last) */
    int  nMasterTel;   /* TEL and EMAIL lines in the master (the first five are the Palm's) */
    int  ncomp;        /* components of the master's type (>1: a series with moved occurrences) */
    int  ntz;          /* VTIMEZONE TZIDs present */
    char tz[4][64];
} ObjScan;

/* kind: KIND_CARD / KIND_CAL / KIND_TODO (sync.h). `len` < 0: to end of file. */
int obj_scan(int kind, FILE *f, long off, long len, ObjScan *sc);

/* The Palm view into out (NUL-terminated). Returns its length, cap-1 if it
 * didn't fit (read that as "too big", as a truncated fetch always was), or -1. */
int obj_filter(int kind, FILE *f, long off, long len, const ObjScan *sc,
               char *out, int cap);

/* The merged object to `out`. eSrv / eLoc: the component the Palm emitter makes
 * from the server's object and from the device's record (BEGIN:VEVENT..END:VEVENT
 * and so on, no VCALENDAR or VTIMEZONE around it). vtz: the device zone's
 * VTIMEZONE block (or "") and tzid its id (or ""), added if a changed time needs
 * a zone the object lacks. Returns the number of groups replaced (0: nothing to
 * send), or -1: unreadable, or an edit refused because it would do harm (the
 * start or repeat of a series that has moved occurrences, which are tied to
 * the series' original times). */
int obj_merge(int kind, FILE *f, long off, long len, const ObjScan *sc,
              const char *eSrv, const char *eLoc, const char *vtz, const char *tzid,
              FILE *out);

#endif
