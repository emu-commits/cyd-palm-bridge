/* palm.h -- shared data model + module seams for the CYD Palm<->DAV bridge.
 *
 * Design rule (carried from the spikes): the database is NEVER held in RAM.
 * PDB is streamed one record at a time; DAV objects are streamed one object
 * at a time. The only resident state is the tiny sync map (uniqueID<->href).
 *
 * This header is host/ESP32-agnostic: pure C99, no malloc-heavy containers,
 * fixed-size record buffers sized for the largest realistic Palm record.
 */
#ifndef PALM_H
#define PALM_H
#include <stdint.h>
#include <stddef.h>

#define PALM_REC_MAX 4096          /* max single packed record we handle   */
#define PALM_EPOCH_1904 2082844800u /* seconds from unix epoch to 1904 base */

/* ---- record index attributes (high nibble of the 8-byte index entry) ---- */
#define REC_ATTR_DELETE  0x80
#define REC_ATTR_DIRTY   0x40
#define REC_ATTR_BUSY    0x20
#define REC_ATTR_SECRET  0x10
#define REC_ATTR_CAT     0x0F      /* low nibble = category id              */

/* ================= big-endian byte helpers (68k on-disk order) ========== */
static inline uint16_t be16(const uint8_t*p){ return (uint16_t)((p[0]<<8)|p[1]); }
static inline uint32_t be32(const uint8_t*p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3]; }
static inline void put16(uint8_t*p,uint16_t v){ p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static inline void put32(uint8_t*p,uint32_t v){ p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

/* ============================ DateBook ================================== */
enum { repeatNone, repeatDaily, repeatWeekly, repeatMonthlyByDay,
       repeatMonthlyByDate, repeatYearly };

typedef struct {
    int  hasTime;                 /* 0 == untimed/all-day                   */
    int  sH, sM, eH, eM;          /* start/end wall-clock (Palm has no TZ)  */
    int  year, month, day;        /* Gregorian start date                   */
    int  hasAlarm; int alarmAdv; int alarmUnit; /* advance + unit (0=min..) */
    int  hasRepeat;
    int  repeatType, repeatFreq, repeatOn, repeatForever;
    int  endYear, endMonth, endDay;
    int  startOfWeek;
    int  nExcept; struct { int y,m,d; } excpt[16];
    char description[256];
    char note[512];
} Appt;

/* both directions live in datebook.c */
int  ApptUnpack(const uint8_t *r, int len, Appt *a);        /* bytes -> Appt */
int  ApptPack(uint8_t *buf, int cap, const Appt *a);        /* Appt -> bytes, returns len or -1 */

/* ============================ AddressBook =============================== */
enum { F_name, F_firstName, F_company, F_phone1, F_phone2, F_phone3, F_phone4,
       F_phone5, F_address, F_city, F_state, F_zip, F_country, F_title,
       F_custom1, F_custom2, F_custom3, F_custom4, F_note, F_COUNT };

enum { workLabel, homeLabel, faxLabel, otherLabel, emailLabel, mainLabel,
       pagerLabel, mobileLabel };

typedef struct {
    char store[1024];             /* backing store for field strings        */
    int  used;
    const char *fields[F_COUNT];  /* into store, or NULL                     */
    int  phoneLabel[5];           /* label index per phone slot              */
    int  displayPhone;            /* which phone slot is the "shown" one     */
} Addr;

int  AddrUnpack(const uint8_t *r, int len, Addr *a);        /* bytes -> Addr */
int  AddrPack(uint8_t *buf, int cap, const Addr *a);        /* Addr -> bytes */
/* helper for parsers building an Addr: copy s into store, return stable ptr */
const char *AddrIntern(Addr *a, const char *s);

/* ============================ iCalendar ================================= */
void ical_set_tz(const char *tzid);      /* configure device zone; NULL/UTC = floating */
int  ical_vtimezone(char *out, int cap); /* VTIMEZONE block for current zone, or 0 */
int  ical_emit(char *out, int cap, const Appt *a, uint32_t uid);  /* Appt->VEVENT */
int  ical_parse(const char *ics, Appt *a);                        /* VEVENT->Appt, 0 ok */

/* ============================ vCard ==================================== */
int  vcard_emit(char *out, int cap, const Addr *a, uint32_t uid); /* Addr->VCARD */
int  vcard_parse(const char *vcf, Addr *a);                       /* VCARD->Addr, 0 ok */

/* ============================ PDB container ============================= */
typedef struct {
    uint8_t  attr;
    uint32_t uniqueID;            /* 24-bit                                  */
    const uint8_t *data;          /* points into a reader-owned buffer       */
    int      len;
} PdbRec;

/* streaming reader: cb called once per record; DB never fully resident.
 * return nonzero from cb to stop early. returns record count or -1.        */
typedef int (*pdb_rec_cb)(const PdbRec *rec, int index, void *ctx);
int pdb_read(const char *path, pdb_rec_cb cb, void *ctx);

/* writer: build a PDB from an array of records (records themselves are the
 * only sizable data; the array of PdbRec is small). type/creator are 4cc.  */
int pdb_write(const char *path, const char *name,
              uint32_t type, uint32_t creator,
              const PdbRec *recs, int nrecs);

#endif
