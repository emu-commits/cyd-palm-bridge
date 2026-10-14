/* fidelity.c -- an edit on the device reaches the server without losing
 * anything the Palm can't hold. End to end, against Radicale.
 *
 * Built with the device's 8 KB fetch buffer (-DOBJ_FETCH_CAP=8192), so the
 * 40 KB photo contact below is also the proof that a big contact comes down at
 * all: before the Palm view (objfile.h) it was dropped with a warning.
 *
 * Each case puts an iCloud-shaped object on the server the way a phone would,
 * syncs it down, edits it on the "device", syncs again, and compares the
 * server's copy before and after: every line except the edited field must
 * still be there.
 *
 * Needs Radicale with palm/cal and palm/card (run_gates.sh sets it up).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"
#include "radicale_rm.h"

static DavCtx D;
static int fails = 0;
static void CK(int c, const char *m){ printf("  %s %s\n", c ? "ok  " : "FAIL", m); if(!c) fails++; }

typedef struct { char n[64][256]; int c; } NL;
static void nlCb(const char *name, const char *etag, void *ctx){
    (void)etag; NL *l = ctx; if(l->c < 64) snprintf(l->n[l->c++], 256, "%s", name);
}
static void clearColl(const char *coll){
    NL *l = calloc(1, sizeof *l); dav_list(&D, coll, nlCb, l);
    for(int i = 0; i < l->c; i++) if(strstr(l->n[i], ".ics") || strstr(l->n[i], ".vcf")) test_rm(&D, coll, l->n[i], NULL);
    free(l);
}
static void putRaw(const char *coll, const char *name, const char *ctype, const char *body){
    FILE *f = fopen("state/.fid", "wb"); fputs(body, f); fclose(f);
    char etag[160]; int st = 0;
    dav_put(&D, coll, name, ctype, "state/.fid", NULL, etag, sizeof etag, &st);
    if(st < 200 || st >= 300) printf("  (put %s -> %d)\n", name, st);
}
/* the server's copy, unfolded */
static char *getUnfolded(const char *coll, const char *name){
    char *b = malloc(256 * 1024);
    int n = dav_get(&D, coll, name, b, 256 * 1024); if(n < 0) n = 0; b[n] = 0;
    char *o = b, *p = b;
    while(*p){
        if(p[0] == '\r' && p[1] == '\n' && (p[2] == ' ' || p[2] == '\t')){ p += 3; continue; }
        if(p[0] == '\n' && (p[1] == ' ' || p[1] == '\t')){ p += 2; continue; }
        *o++ = *p++;
    }
    *o = 0;
    return b;
}
/* every line of `before` not starting with one of `except` is a line of `after` */
static int allKept(const char *before, const char *after0, const char *const *except){
    int ok = 1; const char *p = before;
    char *after = malloc(strlen(after0) + 2); after[0] = '\n'; strcpy(after + 1, after0);
    while(*p){
        const char *e = strpbrk(p, "\r\n"); size_t l = e ? (size_t)(e - p) : strlen(p);
        int skip = l == 0;
        for(const char *const *x = except; *x && !skip; x++) if(!strncmp(p, *x, strlen(*x))) skip = 1;
        if(!skip){
            char *ln = malloc(l + 3); ln[0] = '\n'; memcpy(ln + 1, p, l); ln[l + 1] = 0;
            char *hit = strstr(after, ln);
            if(!hit || (hit[l + 1] != '\r' && hit[l + 1] != '\n')){ if(ok) printf("    lost: %.*s\n", (int)(l > 90 ? 90 : l), p); ok = 0; }
            free(ln);
        }
        p += l; while(*p == '\r' || *p == '\n') p++;
    }
    free(after);
    return ok;
}

/* ---- the device's database, whole ---- */
typedef struct { uint32_t uid; uint8_t attr; int len; uint8_t data[PALM_REC_MAX]; } Rec;
static Rec recs[16]; static int nrecs;
static int loadCb(const PdbRec *r, int i, void *c){
    (void)i; (void)c;
    if(nrecs < 16){ Rec *x = &recs[nrecs++]; x->uid = r->uniqueID; x->attr = r->attr; x->len = r->len; memcpy(x->data, r->data, (size_t)r->len); }
    return 0;
}
static void loadLocal(const char *pdb){ nrecs = 0; pdb_read(pdb, loadCb, NULL); }
static void saveLocal(const char *pdb, const char *name, uint32_t type){
    static PdbRec r[16];
    for(int i = 0; i < nrecs; i++) r[i] = (PdbRec){ .attr = recs[i].attr, .uniqueID = recs[i].uid, .data = recs[i].data, .len = recs[i].len };
    pdb_write(pdb, name, 0x44415441, type, r, nrecs);
}
static int ops(const SyncStats *s){ return s->pushNew + s->pushMod + s->pullNew + s->pullMod + s->pullDel + s->conflicts; }

/* ------------------------------------------------------------------ events */
#define CAL  "palm/cal"
#define CMAP "state/fid-cal.map"
#define CPDB "pdb/fid-cal.pdb"
static const char *EVENT =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Apple Inc.//iPhone OS 18.0//EN\r\nCALSCALE:GREGORIAN\r\n"
    "BEGIN:VEVENT\r\n"
    "CREATED:20160301T101010Z\r\n"
    "UID:FID-EVENT-0001\r\n"
    "DTSTART:20160307T090000\r\n"
    "DTEND:20160307T100000\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=MO\r\n"
    "SUMMARY:Team sync\r\n"
    "LOCATION:Room 4\\, 2nd floor\r\n"
    "ORGANIZER;CN=Pat:mailto:pat@example.com\r\n"
    "ATTENDEE;CN=Sam;PARTSTAT=ACCEPTED:mailto:sam@example.com\r\n"
    "ATTENDEE;CN=Alex;PARTSTAT=NEEDS-ACTION;ROLE=REQ-PARTICIPANT:mailto:alex@example.com\r\n"
    "URL;VALUE=URI:https://example.com/meet/42\r\n"
    "X-APPLE-TRAVEL-ADVISORY-BEHAVIOR:AUTOMATIC\r\n"
    "SEQUENCE:3\r\n"
    "BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:Reminder\r\nTRIGGER:-PT15M\r\nX-WR-ALARMUID:A1\r\nEND:VALARM\r\n"
    "BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:Reminder\r\nTRIGGER:-P1D\r\nX-WR-ALARMUID:A2\r\nEND:VALARM\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "UID:FID-EVENT-0001\r\n"
    "RECURRENCE-ID:20160321T090000\r\n"
    "DTSTART:20160322T140000\r\n"
    "DTEND:20160322T150000\r\n"
    "SUMMARY:Team sync (moved)\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n";

static void events(void){
    printf("== an iCloud event edited on the device ==\n");
    ical_set_tz(NULL);
    clearColl(CAL); remove(CMAP);
    pdb_write(CPDB, "DatebookDB", 0x44415441, 0x64617465, NULL, 0);
    putRaw(CAL, "fid-ev.ics", "text/calendar; charset=utf-8", EVENT);

    SyncStats s0 = {0}; sync_collection(&D, CPDB, CPDB, CAL, KIND_CAL, CMAP, POL_SERVER, &s0);
    loadLocal(CPDB);
    Appt a; int got = nrecs == 1 && !ApptUnpack(recs[0].data, recs[0].len, &a);
    CK(s0.pullNew == 1 && got, "pulled down");
    CK(got && !strcmp(a.description, "Team sync") && a.day == 7, "as the series, not its moved occurrence");

    char *before = getUnfolded(CAL, "fid-ev.ics");
    snprintf(a.description, sizeof a.description, "Team sync, renamed");
    recs[0].len = ApptPack(recs[0].data, PALM_REC_MAX, &a); saveLocal(CPDB, "DatebookDB", 0x64617465);
    SyncStats s1 = {0}; sync_collection(&D, CPDB, CPDB, CAL, KIND_CAL, CMAP, POL_SERVER, &s1);
    char *after = getUnfolded(CAL, "fid-ev.ics");
    CK(s1.pushMod == 1 && s1.unsent == 0, "the edit went up");
    CK(strstr(after, "SUMMARY:Team sync\\, renamed") != NULL, "the server has it");
    const char *ex[] = {"SUMMARY:Team sync\r", "SUMMARY:Team sync\n", "DTSTAMP", "LAST-MODIFIED", NULL};
    CK(allKept(before, after, ex), "and every other line: attendees, location, URL, both alarms, the moved occurrence");
    free(before); free(after);

    SyncStats s2 = {0}; sync_collection(&D, CPDB, CPDB, CAL, KIND_CAL, CMAP, POL_SERVER, &s2);
    CK(ops(&s2) == 0, "the next sync is a no-op");

    printf("== edited on both sides, the device's policy wins ==\n");
    before = getUnfolded(CAL, "fid-ev.ics");
    char *ph = malloc(64 * 1024); snprintf(ph, 64 * 1024, "%s", before);
    char *at = strstr(ph, "ATTENDEE;CN=Sam");
    if(at){ char tail[32768]; snprintf(tail, sizeof tail, "%s", at);
            snprintf(at, 64 * 1024 - (size_t)(at - ph), "ATTENDEE;CN=Robin;PARTSTAT=TENTATIVE:mailto:robin@example.com\r\n%s", tail); }
    putRaw(CAL, "fid-ev.ics", "text/calendar; charset=utf-8", ph);   /* the phone adds a guest */
    free(ph); free(before);
    loadLocal(CPDB); ApptUnpack(recs[0].data, recs[0].len, &a);
    snprintf(a.note, sizeof a.note, "Bring the Q3 numbers");
    recs[0].len = ApptPack(recs[0].data, PALM_REC_MAX, &a); saveLocal(CPDB, "DatebookDB", 0x64617465);
    SyncStats s3 = {0}; sync_collection(&D, CPDB, CPDB, CAL, KIND_CAL, CMAP, POL_LOCAL, &s3);
    after = getUnfolded(CAL, "fid-ev.ics");
    CK(s3.conflicts == 1, "a conflict");
    CK(strstr(after, "DESCRIPTION:Bring the Q3 numbers") != NULL, "the device's note went up");
    CK(strstr(after, "CN=Robin") && strstr(after, "CN=Sam") && strstr(after, "TRIGGER:-P1D"),
       "and the guest the phone just added is still there (merged, not replaced)");
    free(after);

    printf("== an edit with no base (made before this firmware's first sync) ==\n");
    before = getUnfolded(CAL, "fid-ev.ics");
    remove(CMAP ".bdat"); remove(CMAP ".bidx");
    loadLocal(CPDB); ApptUnpack(recs[0].data, recs[0].len, &a);
    char was[256]; snprintf(was, sizeof was, "%s", a.description);
    snprintf(a.description, sizeof a.description, "Edited before the update");
    recs[0].len = ApptPack(recs[0].data, PALM_REC_MAX, &a); saveLocal(CPDB, "DatebookDB", 0x64617465);
    SyncStats s5 = {0}; sync_collection(&D, CPDB, CPDB, CAL, KIND_CAL, CMAP, POL_SERVER, &s5);
    after = getUnfolded(CAL, "fid-ev.ics");
    CK(s5.conflicts == 1 && s5.pushMod == 0, "handled as a conflict, nothing pushed");
    CK(!strcmp(before, after), "the server's copy is exactly as it was");
    loadLocal(CPDB); ApptUnpack(recs[0].data, recs[0].len, &a);
    CK(!strcmp(a.description, was), "the device took the server's copy (the default policy)");
    free(before); free(after);
    FILE *bf = fopen(CMAP ".bidx", "rb"); CK(bf != NULL, "and the base is written again"); if(bf) fclose(bf);

    printf("== the series moved on the device: it has a moved occurrence, so refused ==\n");
    before = getUnfolded(CAL, "fid-ev.ics");
    loadLocal(CPDB); ApptUnpack(recs[0].data, recs[0].len, &a);
    a.sH = 11; a.eH = 12;
    recs[0].len = ApptPack(recs[0].data, PALM_REC_MAX, &a); saveLocal(CPDB, "DatebookDB", 0x64617465);
    SyncStats s4 = {0}; sync_collection(&D, CPDB, CPDB, CAL, KIND_CAL, CMAP, POL_SERVER, &s4);
    after = getUnfolded(CAL, "fid-ev.ics");
    CK(s4.unsent == 1 && s4.pushMod == 0, "counted as unsent, nothing pushed");
    CK(!strcmp(before, after), "the server's copy is exactly as it was");
    loadLocal(CPDB); ApptUnpack(recs[0].data, recs[0].len, &a);
    CK(a.sH == 11, "and the edit is still on the device, to try again");
    free(before); free(after);
}

/* ---------------------------------------------------------------- contacts */
#define CARD "palm/card"
#define AMAP "state/fid-card.map"
#define APDB "pdb/fid-card.pdb"
static char *bigCard(void){
    size_t cap = 64 * 1024; char *b = malloc(cap); size_t n = 0;
    n += (size_t)snprintf(b + n, cap - n,
        "BEGIN:VCARD\r\nVERSION:3.0\r\nPRODID:-//Apple Inc.//iPhone OS 18.0//EN\r\n"
        "N:Rivera;Jordan;;;\r\nFN:Jordan Rivera\r\nORG:Acme;Research\r\n"
        "item1.EMAIL;type=INTERNET;type=pref:jordan@example.com\r\n"
        "item1.X-ABLabel:_$!<Other>!$_\r\n"
        "TEL;type=CELL;type=VOICE;type=pref:+1 555 0101\r\n"
        "TEL;type=WORK;type=VOICE:+1 555 0102\r\n"
        "TEL;type=HOME;type=VOICE:+1 555 0103\r\n"
        "EMAIL;type=INTERNET;type=HOME:jr@home.example\r\n"
        "TEL;type=MAIN:+1 555 0105\r\n"
        "TEL;type=IPHONE:+1 555 0106\r\n"
        "TEL;type=PAGER:+1 555 0107\r\n"
        "item2.ADR;type=HOME:;;1 Home St;Springfield;IL;62701;USA\r\n"
        "ADR;type=WORK;type=pref:;;9 Work Ave;Chicago;IL;60601;USA\r\n"
        "BDAY;value=date:1980-04-02\r\n"
        "URL;type=HOME:https://jordan.example\r\n"
        "UID:FID-CARD-0001\r\n"
        "PHOTO;ENCODING=b;TYPE=JPEG:");
    for(int i = 0; i < 40 * 1024 / 74; i++){
        for(int k = 0; k < 74; k++) b[n++] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(i * 7 + k) % 64];
        memcpy(b + n, "\r\n ", 3); n += 3;
    }
    n += (size_t)snprintf(b + n, cap - n, "QQ==\r\nNOTE:Met at the 2016 offsite\r\nEND:VCARD\r\n");
    return b;
}

static void contacts(void){
    printf("== a 40 KB photo contact, with the device's 8 KB fetch buffer ==\n");
    clearColl(CARD); remove(AMAP);
    pdb_write(APDB, "AddressDB", 0x44415441, 0x61646472, NULL, 0);
    char *card = bigCard();
    putRaw(CARD, "fid-card.vcf", "text/vcard; charset=utf-8", card);
    free(card);
    SyncStats s0 = {0}; sync_collection(&D, APDB, APDB, CARD, KIND_CARD, AMAP, POL_SERVER, &s0);
    loadLocal(APDB);
    Addr a; int got = nrecs == 1 && !AddrUnpack(recs[0].data, recs[0].len, &a);
    CK(s0.pullNew == 1 && got, "it comes down (it used to be dropped as too big)");
    CK(got && a.fields[F_phone1] && !strcmp(a.fields[F_phone1], "jordan@example.com"),
       "with Apple's item1.EMAIL in the first slot");

    char *before = getUnfolded(CARD, "fid-card.vcf");
    /* the slot holding the mobile number (Radicale re-serializes, so not by position) */
    int slot = -1;
    for(int i = 0; i < 5; i++) if(a.fields[F_phone1+i] && !strcmp(a.fields[F_phone1+i], "+1 555 0101")) slot = i;
    CK(slot >= 0, "the mobile number is one of the Palm's five slots");
    if(slot >= 0) a.fields[F_phone1+slot] = AddrIntern(&a, "+1 555 0999");
    recs[0].len = AddrPack(recs[0].data, PALM_REC_MAX, &a); saveLocal(APDB, "AddressDB", 0x61646472);
    SyncStats s1 = {0}; sync_collection(&D, APDB, APDB, CARD, KIND_CARD, AMAP, POL_SERVER, &s1);
    char *after = getUnfolded(CARD, "fid-card.vcf");
    CK(s1.pushMod == 1, "the edited number went up");
    CK(strstr(after, "+1 555 0999") && !strstr(after, "+1 555 0101"), "the server has it");
    const char *ex[] = {"TEL;TYPE=CELL", "REV", NULL};
    CK(allKept(before, after, ex), "and everything else: the photo, birthday, URL, labels, the other numbers, both addresses");
    free(before); free(after);
    SyncStats s2 = {0}; sync_collection(&D, APDB, APDB, CARD, KIND_CARD, AMAP, POL_SERVER, &s2);
    CK(ops(&s2) == 0, "the next sync is a no-op");
}

/* ---------------------------------------------------------------- read-only */
static int countColl(const char *coll){ NL *l = calloc(1, sizeof *l); dav_list(&D, coll, nlCb, l);
    int n = 0; for(int i = 0; i < l->c; i++) if(strstr(l->n[i], ".ics")) n++; free(l); return n; }
static int findDesc(const char *d){
    loadLocal(CPDB);
    for(int i = 0; i < nrecs; i++){ Appt a; if(!ApptUnpack(recs[i].data, recs[i].len, &a) && !strcmp(a.description, d)) return i; }
    return -1;
}
static void readOnly(void){
    printf("== read-only: the account is never changed ==\n");
    clearColl(CAL); remove(CMAP);
    pdb_write(CPDB, "DatebookDB", 0x44415441, 0x64617465, NULL, 0);
    const char *A = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:RO-A\r\nDTSTART:20261102T090000\r\n"
                    "DTEND:20261102T100000\r\nSUMMARY:Alpha\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    const char *B = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:RO-B\r\nDTSTART:20261103T090000\r\n"
                    "DTEND:20261103T100000\r\nSUMMARY:Bravo\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
    putRaw(CAL, "ro-a.ics", "text/calendar; charset=utf-8", A);
    putRaw(CAL, "ro-b.ics", "text/calendar; charset=utf-8", B);
    sync_set_pull_only(1);
    SyncStats s0 = {0}; sync_collection(&D, CPDB, CPDB, CAL, KIND_CAL, CMAP, POL_SERVER, &s0);
    CK(s0.pullNew == 2, "read-only still brings the account down");

    /* the device edits Alpha and makes a new event; the phone edits Bravo */
    int ia = findDesc("Alpha"); Appt a;
    if(ia >= 0){ ApptUnpack(recs[ia].data, recs[ia].len, &a); snprintf(a.description, sizeof a.description, "Alpha, edited here");
                 recs[ia].len = ApptPack(recs[ia].data, PALM_REC_MAX, &a); }
    memset(&a, 0, sizeof a); a.hasTime = 1; a.sH = 8; a.eH = 9; a.year = 2026; a.month = 11; a.day = 4;
    snprintf(a.description, sizeof a.description, "Charlie, new here");
    Rec *x = &recs[nrecs++]; x->uid = 777; x->attr = 0; x->len = ApptPack(x->data, PALM_REC_MAX, &a);
    saveLocal(CPDB, "DatebookDB", 0x64617465);
    char *beforeA = getUnfolded(CAL, "ro-a.ics");
    char *B2 = strdup(B); memcpy(strstr(B2, "Bravo"), "BRAVO", 5);
    putRaw(CAL, "ro-b.ics", "text/calendar; charset=utf-8", B2); free(B2);

    SyncStats s1 = {0}; sync_collection(&D, CPDB, CPDB, CAL, KIND_CAL, CMAP, POL_SERVER, &s1);
    char *afterA = getUnfolded(CAL, "ro-a.ics");
    CK(s1.pushNew == 0 && s1.pushMod == 0, "nothing pushed");
    CK(!strcmp(beforeA, afterA) && countColl(CAL) == 2, "the server is exactly as it was: Alpha unedited, no Charlie");
    CK(findDesc("Alpha, edited here") >= 0 && findDesc("Charlie, new here") >= 0, "both changes wait on the device");
    CK(s1.pullMod == 1 && findDesc("BRAVO") >= 0, "and the phone's edit still comes down");
    free(beforeA); free(afterA);

    printf("== read-only turned off: what waited goes up ==\n");
    sync_set_pull_only(0);
    SyncStats s2 = {0}; sync_collection(&D, CPDB, CPDB, CAL, KIND_CAL, CMAP, POL_SERVER, &s2);
    afterA = getUnfolded(CAL, "ro-a.ics");
    CK(s2.pushMod == 1 && s2.pushNew == 1, "the edit and the new event are sent");
    CK(strstr(afterA, "SUMMARY:Alpha\\, edited here") != NULL && countColl(CAL) == 3, "and the server has both");
    free(afterA);
}

int main(void){
    snprintf(D.base, sizeof D.base, "%s", getenv("DAV_BASE") ? getenv("DAV_BASE") : "http://localhost:5232");
    snprintf(D.user, sizeof D.user, "palm"); snprintf(D.pass, sizeof D.pass, "palm");
    events();
    contacts();
    readOnly();
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
