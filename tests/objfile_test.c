/* objfile_test.c -- an edit on the device must not lose what the Palm can't hold.
 *
 * Offline. iCloud-shaped objects -- a weekly series with a moved occurrence,
 * attendees, a location and two alarms; a contact with a 40 KB photo, eight
 * phone numbers, two addresses and a birthday; a reminder with Apple's own
 * properties -- go through the same steps the engine takes for a device edit:
 * obj_scan, obj_filter (the Palm view), parse, change one field the way the
 * device would, emit both, obj_merge. Every line of the server's object that
 * isn't the changed field must come out byte for byte.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/palm.h"
#include "../bridge/sync.h"
#include "../bridge/objfile.h"

static int fails = 0;
static void CK(int c, const char *m){ printf("   %s %s\n", c ? "ok  " : "FAIL", m); if(!c) fails++; }

static FILE *tmpWith(const char *s){ FILE *f = tmpfile(); fputs(s, f); rewind(f); return f; }
static char *slurpF(FILE *f){
    fflush(f); long n = ftell(f); rewind(f);
    char *b = malloc((size_t)n + 1); size_t g = fread(b, 1, (size_t)n, f); b[g] = 0; return b;
}
/* does `hay` contain line `ln` (CRLF-ended) exactly? */
static int hasLine(const char *hay, const char *ln){
    char w[4200]; snprintf(w, sizeof w, "\n%s\r\n", ln); return strstr(hay, w) != NULL;
}
/* every CRLF line of a that isn't in `except` (prefixes) appears in b, in order */
static int keptInOrder(const char *a, const char *b, const char *const *except){
    const char *p = a, *q = b;
    while(*p){
        const char *e = strstr(p, "\r\n"); if(!e) break;
        size_t l = (size_t)(e - p + 2);
        int skip = 0; for(const char *const *x = except; x && *x; x++) if(!strncmp(p, *x, strlen(*x))) skip = 1;
        if(!skip){
            char ln[4200]; if(l >= sizeof ln) return 0; memcpy(ln, p, l); ln[l] = 0;
            const char *at = strstr(q, ln); if(!at){ printf("      missing: %.*s", (int)l, p); return 0; }
            q = at + l;
        }
        p += l;
    }
    return 1;
}

/* ------------------------------------------------------------------ events */
static const char *EVENT =
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "PRODID:-//Apple Inc.//iPhone OS 18.0//EN\r\n"
    "CALSCALE:GREGORIAN\r\n"
    "BEGIN:VTIMEZONE\r\n"
    "TZID:Europe/London\r\n"
    "BEGIN:STANDARD\r\n"
    "DTSTART:19961027T020000\r\n"
    "TZOFFSETFROM:+0100\r\n"
    "TZOFFSETTO:+0000\r\n"
    "END:STANDARD\r\n"
    "END:VTIMEZONE\r\n"
    "BEGIN:VEVENT\r\n"
    "CREATED:20160301T101010Z\r\n"
    "UID:6A1B2C3D-AAAA-BBBB-CCCC-1234567890AB\r\n"
    "DTSTART;TZID=America/New_York:20160307T090000\r\n"
    "DTEND;TZID=America/New_York:20160307T100000\r\n"
    "RRULE:FREQ=WEEKLY;BYDAY=MO\r\n"
    "EXDATE;TZID=America/New_York:20160314T090000\r\n"
    "SUMMARY:Team sync\r\n"
    "LOCATION:Room 4\\, 2nd floor\r\n"
    "ORGANIZER;CN=Pat:mailto:pat@example.com\r\n"
    "ATTENDEE;CN=Sam;PARTSTAT=ACCEPTED:mailto:sam@example.com\r\n"
    "ATTENDEE;CN=Alex Long-Name-That-Folds;PARTSTAT=NEEDS-ACTION;ROLE=REQ-PARTICIP\r\n"
    " ANT:mailto:alex@example.com\r\n"
    "URL;VALUE=URI:https://example.com/meet/42\r\n"
    "X-APPLE-TRAVEL-ADVISORY-BEHAVIOR:AUTOMATIC\r\n"
    "SEQUENCE:3\r\n"
    "BEGIN:VALARM\r\n"
    "ACTION:DISPLAY\r\n"
    "DESCRIPTION:Reminder\r\n"
    "TRIGGER:-PT15M\r\n"
    "X-WR-ALARMUID:11111111-2222\r\n"
    "END:VALARM\r\n"
    "BEGIN:VALARM\r\n"
    "ACTION:DISPLAY\r\n"
    "DESCRIPTION:Reminder\r\n"
    "TRIGGER:-P1D\r\n"
    "END:VALARM\r\n"
    "END:VEVENT\r\n"
    "BEGIN:VEVENT\r\n"
    "UID:6A1B2C3D-AAAA-BBBB-CCCC-1234567890AB\r\n"
    "RECURRENCE-ID;TZID=America/New_York:20160321T090000\r\n"
    "DTSTART;TZID=America/New_York:20160322T140000\r\n"
    "DTEND;TZID=America/New_York:20160322T150000\r\n"
    "SUMMARY:Team sync (moved)\r\n"
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n";

static int calView(FILE *f, ObjScan *sc, Appt *a, char *view, int cap){
    obj_scan(KIND_CAL, f, 0, -1, sc);
    int n = obj_filter(KIND_CAL, f, 0, -1, sc, view, cap);
    if(n <= 0 || n >= cap-1) return -1;
    return ical_parse(view, a);
}
static char *calMerge(FILE *f, const ObjScan *sc, const Appt *s, const Appt *l, int *rc){
    static char es[8192], el[8192], vt[2048];
    ical_emit(es, sizeof es, s, 1); ical_emit(el, sizeof el, l, 1);
    int vl = ical_vtimezone(vt, sizeof vt); if(vl < 0) vt[0] = 0;
    FILE *o = tmpfile();
    *rc = obj_merge(KIND_CAL, f, 0, -1, sc, es, es, el, vt, "America/New_York", "", o);
    char *r = slurpF(o); fclose(o); return r;
}

static void events(void){
    printf("== a series with a moved occurrence, attendees, two alarms ==\n");
    ical_set_tz("America/New_York");
    FILE *f = tmpWith(EVENT); ObjScan sc; Appt s; char view[8192];
    CK(calView(f, &sc, &s, view, sizeof view) == 0, "the Palm view parses");
    CK(sc.ok && sc.master == 0, "the master is the VEVENT without a RECURRENCE-ID");
    CK(!strcmp(s.description, "Team sync"), "the series' summary, not the moved occurrence's");
    CK(s.sH == 9 && s.day == 7 && s.hasRepeat, "the series' start and its repeat");
    CK(!strstr(view, "ATTENDEE") && !strstr(view, "moved") && !strstr(view, "TZOFFSET"),
       "the view holds only what the Palm reads");

    printf("== the summary edited on the device ==\n");
    Appt l = s; snprintf(l.description, sizeof l.description, "Team sync, renamed");
    int rc; char *m = calMerge(f, &sc, &s, &l, &rc);
    const char *ex1[] = {"SUMMARY:Team sync\r\n", NULL};
    CK(rc == 1, "one group replaced");
    CK(hasLine(m, "SUMMARY:Team sync\\, renamed"), "the new summary is there");
    CK(!hasLine(m, "SUMMARY:Team sync"), "the old one is gone");
    CK(keptInOrder(EVENT, m, ex1), "every other line kept byte for byte, in order (attendees, URL, both alarms, the override)");
    free(m);

    printf("== nothing edited ==\n");
    m = calMerge(f, &sc, &s, &s, &rc);
    CK(rc == 0 && !strcmp(m, EVENT), "nothing to replace, and the object comes back identical");
    free(m);

    printf("== the time of a series with a moved occurrence: refused ==\n");
    l = s; l.sH = 10; l.eH = 11;
    m = calMerge(f, &sc, &s, &l, &rc);
    CK(rc == -1, "refused: the moved occurrence is tied to the old start");
    free(m);

    printf("== the time edited (a series without moved occurrences) ==\n");
    char *plain = strdup(EVENT);
    { char *ov = strstr(plain, "BEGIN:VEVENT\r\nUID:6A1B2C3D-AAAA-BBBB-CCCC-1234567890AB\r\nRECURRENCE-ID");
      char *end = strstr(ov, "END:VEVENT\r\n") + 12; memmove(ov, end, strlen(end) + 1); }
    fclose(f); f = tmpWith(plain);
    calView(f, &sc, &s, view, sizeof view);
    CK(sc.ncomp == 1, "one VEVENT");
    m = calMerge(f, &sc, &s, &l, &rc);
    const char *ex2[] = {"DTSTART;TZID=America/New_York:20160307T090000", "DTEND;TZID=America/New_York:20160307T100000",
                         "EXDATE", "BEGIN:VTIMEZONE\r\nTZID:America", NULL};
    CK(rc == 2, "the time and the exception dates (they carry the time) replaced");
    CK(hasLine(m, "DTSTART;TZID=America/New_York:20160307T100000"), "the new start");
    CK(strstr(m, "TZID:America/New_York") != NULL, "a VTIMEZONE for the zone the new time names");
    CK(strstr(m, "TZID:Europe/London") != NULL, "and the object's own VTIMEZONE kept");
    CK(keptInOrder(plain, m, ex2), "everything else kept");
    free(m); free(plain);

    printf("== the alarm edited ==\n");
    l = s; l.alarmAdv = 30; l.alarmUnit = 0;
    m = calMerge(f, &sc, &s, &l, &rc);
    CK(rc == 1 && strstr(m, "TRIGGER:-PT30M") && !strstr(m, "TRIGGER:-P1D") && !strstr(m, "-PT15M"),
       "the alarms replaced by the device's one (only when the alarm itself was edited)");
    CK(strstr(m, "ATTENDEE;CN=Sam") && strstr(m, "LOCATION:Room 4"), "attendees and location kept");
    free(m);
    fclose(f);
}

/* ---------------------------------------------------------------- contacts */
static char *bigCard(void){
    size_t cap = 64 * 1024; char *b = malloc(cap); size_t n = 0;
    n += (size_t)snprintf(b + n, cap - n,
        "BEGIN:VCARD\r\nVERSION:3.0\r\nPRODID:-//Apple Inc.//iPhone OS 18.0//EN\r\n"
        "N:Rivera;Jordan;;;\r\nFN:Jordan Rivera\r\nORG:Acme;Research\r\n"
        "item1.EMAIL;type=INTERNET;type=pref:jordan@example.com\r\n"
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
        "UID:9F8E7D6C-1111-2222-3333-444455556666\r\n"
        "PHOTO;ENCODING=b;TYPE=JPEG:");
    for(int i = 0; i < 40 * 1024 / 74; i++){
        for(int k = 0; k < 74; k++) b[n++] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(i * 7 + k) % 64];
        memcpy(b + n, "\r\n ", 3); n += 3;
    }
    n += (size_t)snprintf(b + n, cap - n, "QQ==\r\nNOTE:Met at the 2016 offsite\r\nEND:VCARD\r\n");
    return b;
}

static void contacts(void){
    printf("== a contact with a 40 KB photo, eight numbers, two addresses ==\n");
    char *card = bigCard();
    FILE *f = tmpWith(card); ObjScan sc; char view[8192];
    obj_scan(KIND_CARD, f, 0, -1, &sc);
    int vn = obj_filter(KIND_CARD, f, 0, -1, &sc, view, sizeof view);
    printf("   object %d B, Palm view %d B\n", (int)strlen(card), vn);
    CK(vn > 0 && vn < 1500, "the view fits the device's 8 KB fetch buffer many times over");
    CK(!strstr(view, "PHOTO") && !strstr(view, "BDAY"), "no photo, no birthday in the view");
    Addr s; CK(vcard_parse(view, &s) == 0, "it parses");
    CK(s.fields[F_phone1] && !strcmp(s.fields[F_phone1], "jordan@example.com"), "slot 1 is the first EMAIL (Apple's item1.EMAIL)");
    CK(s.fields[F_city] && !strcmp(s.fields[F_city], "Chicago"), "the Palm's address is the last ADR");
    CK(sc.nMasterAdr == 2, "two ADR lines counted");

    printf("== the second phone number edited on the device ==\n");
    uint8_t pk[2048]; int pl = AddrPack(pk, sizeof pk, &s);
    Addr l; AddrUnpack(pk, pl, &l);
    l.fields[F_phone2] = AddrIntern(&l, "+1 555 0999");
    static char es[8192], el[8192];
    vcard_emit(es, sizeof es, &s, 1); vcard_emit(el, sizeof el, &l, 1);
    FILE *o = tmpfile();
    int rc = obj_merge(KIND_CARD, f, 0, -1, &sc, es, es, el, "", "", "", o);
    char *m = slurpF(o); fclose(o);
    const char *ex[] = {"TEL;type=CELL", NULL};
    CK(rc == 1, "one group replaced (the five phone slots)");
    CK(strstr(m, "+1 555 0999") && !strstr(m, "+1 555 0101"), "the new number in, the old one out");
    CK(strstr(m, "item1.EMAIL;type=INTERNET;type=pref:jordan@example.com") && strstr(m, "TEL;type=WORK;type=VOICE:+1 555 0102"),
       "the slots that weren't edited keep their lines exactly (TYPE, PREF, item label)");
    CK(strstr(m, "TEL;type=MAIN:+1 555 0105") && strstr(m, "TEL;type=IPHONE:+1 555 0106") && strstr(m, "TEL;type=PAGER:+1 555 0107"),
       "the sixth to eighth numbers, which the Palm never had, are kept");
    CK(keptInOrder(card, m, ex), "the photo, birthday, URL and both addresses kept byte for byte");
    free(m);

    printf("== the work address edited ==\n");
    l = s; l.fields[F_city] = AddrIntern(&l, "Evanston");
    vcard_emit(el, sizeof el, &l, 1);
    o = tmpfile(); rc = obj_merge(KIND_CARD, f, 0, -1, &sc, es, es, el, "", "", "", o);
    m = slurpF(o); fclose(o);
    CK(rc == 1 && strstr(m, "Evanston") && !strstr(m, "9 Work Ave;Chicago"), "the last ADR replaced");
    CK(strstr(m, "item2.ADR;type=HOME:;;1 Home St;Springfield") != NULL, "the home address kept");
    free(m); free(card); fclose(f);
}

static char *cardMerge(const char *card, const char *addPhone, Addr *sOut){
    FILE *f = tmpWith(card); ObjScan sc; char view[4096];
    obj_scan(KIND_CARD, f, 0, -1, &sc); obj_filter(KIND_CARD, f, 0, -1, &sc, view, sizeof view);
    Addr s; vcard_parse(view, &s); if(sOut) *sOut = s;
    Addr l = s; int i = 0; while(i < 5 && l.fields[F_phone1+i]) i++;
    l.fields[F_phone1+i] = AddrIntern(&l, addPhone); l.phoneLabel[i] = 7;   /* mobile */
    static char es[4096], el[4096]; vcard_emit(es, sizeof es, &s, 1); vcard_emit(el, sizeof el, &l, 1);
    FILE *o = tmpfile(); obj_merge(KIND_CARD, f, 0, -1, &sc, es, es, el, "", "", "", o);
    char *m = slurpF(o); fclose(o); fclose(f); return m;
}
/* A record pulled by an older parser that couldn't see "item1.EMAIL": its base
 * and its present both lack that email. Editing the note must change the note
 * and nothing else -- the 2-way diff against the server's copy would have
 * rewritten the phone slots and dropped the email. */
static void staleBase(void){
    printf("== a record pulled by an older parser, its note edited ==\n");
    char *card = bigCard();
    FILE *f = tmpWith(card); ObjScan sc; char view[8192];
    obj_scan(KIND_CARD, f, 0, -1, &sc); obj_filter(KIND_CARD, f, 0, -1, &sc, view, sizeof view);
    Addr srv; vcard_parse(view, &srv);
    /* the old parser's record: the same, without the grouped email (slot 1) */
    char *old = strdup(view); char *g = strstr(old, "item1.EMAIL"); char *e = strstr(g, "\r\n") + 2;
    memmove(g, e, strlen(e) + 1);
    Addr base; vcard_parse(old, &base);
    CK(base.fields[F_phone1] && !strcmp(base.fields[F_phone1], "+1 555 0101"), "the old record's slot 1 is the mobile");
    Addr now = base; now.fields[F_note] = AddrIntern(&now, "Met at the 2016 offsite; owes me lunch");
    static char eb[8192], es[8192], el[8192];
    vcard_emit(eb, sizeof eb, &base, 1); vcard_emit(es, sizeof es, &srv, 1); vcard_emit(el, sizeof el, &now, 1);
    FILE *o = tmpfile(); int rc = obj_merge(KIND_CARD, f, 0, -1, &sc, eb, es, el, "", "", "", o);
    char *m = slurpF(o); fclose(o);
    const char *ex[] = {"NOTE:", NULL};
    CK(rc == 1, "one group: the note");
    CK(strstr(m, "owes me lunch") != NULL, "the note went in");
    CK(keptInOrder(card, m, ex), "every phone and email line exactly as it was, item1.EMAIL included");
    free(m); free(old); free(card); fclose(f);

    printf("== the same record, one number cleared on the device ==\n");
    card = bigCard(); f = tmpWith(card);
    obj_scan(KIND_CARD, f, 0, -1, &sc); obj_filter(KIND_CARD, f, 0, -1, &sc, view, sizeof view);
    vcard_parse(view, &srv); base = srv; now = srv;
    now.fields[F_phone3] = NULL;                       /* the work number, slot 3 */
    vcard_emit(eb, sizeof eb, &base, 1); vcard_emit(es, sizeof es, &srv, 1); vcard_emit(el, sizeof el, &now, 1);
    o = tmpfile(); rc = obj_merge(KIND_CARD, f, 0, -1, &sc, eb, es, el, "", "", "", o);
    m = slurpF(o); fclose(o);
    const char *ex2[] = {"TEL;type=WORK", NULL};
    CK(rc == 1 && !strstr(m, "+1 555 0102"), "that number is gone from the contact");
    CK(keptInOrder(card, m, ex2), "and only that line: the shift of the Palm's slots rewrote nothing");
    free(m); free(card); fclose(f);
}

static void phonesAdded(void){
    printf("== a number added on the device ==\n");
    const char *C2 = "BEGIN:VCARD\r\nVERSION:3.0\r\nN:Lee;Kim;;;\r\nTEL;type=HOME:111\r\nTEL;type=WORK:222\r\n"
                     "X-SOCIALPROFILE;type=twitter:x.com/kim\r\nUID:K1\r\nEND:VCARD\r\n";
    char *m = cardMerge(C2, "333", NULL);
    CK(strstr(m, "TEL;type=HOME:111\r\nTEL;type=WORK:222\r\nTEL;TYPE=CELL:333\r\nX-SOCIALPROFILE") != NULL,
       "after the last number, the others untouched");
    free(m);
    const char *C0 = "BEGIN:VCARD\r\nVERSION:3.0\r\nN:Lee;Kim;;;\r\nUID:K1\r\nEND:VCARD\r\n";
    m = cardMerge(C0, "333", NULL);
    CK(strstr(m, "UID:K1\r\nTEL;TYPE=CELL:333\r\nEND:VCARD") != NULL, "a contact with none gets it before END");
    free(m);
}

/* ------------------------------------------------------------------- to do */
static void todos(void){
    printf("== a reminder ticked off on the device ==\n");
    const char *T =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Apple Inc.//iOS//EN\r\n"
        "BEGIN:VTODO\r\nUID:REM-1\r\nCREATED:20200101T000000Z\r\nSUMMARY:Buy milk\r\n"
        "DUE;VALUE=DATE:20261020\r\nPRIORITY:5\r\nSTATUS:NEEDS-ACTION\r\n"
        "X-APPLE-SORT-ORDER:123456\r\nBEGIN:VALARM\r\nACTION:DISPLAY\r\nTRIGGER;VALUE=DATE-TIME:20261020T090000Z\r\nEND:VALARM\r\n"
        "END:VTODO\r\nEND:VCALENDAR\r\n";
    FILE *f = tmpWith(T); ObjScan sc; char view[4096];
    obj_scan(KIND_TODO, f, 0, -1, &sc);
    obj_filter(KIND_TODO, f, 0, -1, &sc, view, sizeof view);
    Todo s; CK(vtodo_parse(view, &s) == 0 && !s.completed, "it parses, open");
    Todo l = s; l.completed = 1;
    char es[2048], el[2048]; vtodo_emit(es, sizeof es, &s, 1); vtodo_emit(el, sizeof el, &l, 1);
    FILE *o = tmpfile(); int rc = obj_merge(KIND_TODO, f, 0, -1, &sc, es, es, el, "", "", "20261014T120000Z", o);
    char *m = slurpF(o); fclose(o);
    const char *ex[] = {"STATUS:", NULL};
    CK(rc == 1 && hasLine(m, "STATUS:COMPLETED"), "completed");
    CK(hasLine(m, "COMPLETED:20261014T120000Z"), "with when (COMPLETED), so it stops being listed as open");
    CK(keptInOrder(T, m, ex), "Apple's sort order and the reminder's alarm kept");
    free(m); fclose(f);
}

/* --------------------------------------------------------- hostile shapes */
static void odd(void){
    printf("== an end given as a DURATION ==\n");
    {
        FILE *g = tmpWith("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:d\r\nDTSTART:20260101T093000\r\nDURATION:PT1H45M\r\nSUMMARY:x\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
        ObjScan s2; char v[1024]; Appt a2;
        obj_scan(KIND_CAL, g, 0, -1, &s2); obj_filter(KIND_CAL, g, 0, -1, &s2, v, sizeof v);
        CK(ical_parse(v, &a2) == 0 && a2.eH == 11 && a2.eM == 15, "09:30 for 1h45m ends at 11:15");
        fclose(g);
    }
    printf("== odd objects ==\n");
    ObjScan sc; char view[512];
    FILE *f = tmpWith("garbage\r\nno components here\r\n");
    obj_scan(KIND_CAL, f, 0, -1, &sc);
    CK(!sc.ok && obj_filter(KIND_CAL, f, 0, -1, &sc, view, sizeof view) == -1, "no master: refused");
    fclose(f);
    f = tmpWith("BEGIN:VCALENDAR\nBEGIN:VEVENT\nUID:x\nDTSTART:20260101T090000\nSUMMARY:bare LF line ends\nEND:VEVENT\nEND:VCALENDAR\n");
    Appt a;
    obj_scan(KIND_CAL, f, 0, -1, &sc);
    CK(obj_filter(KIND_CAL, f, 0, -1, &sc, view, sizeof view) > 0 && ical_parse(view, &a) == 0 &&
       !strcmp(a.description, "bare LF line ends"), "bare LF line ends read");
    fclose(f);
    f = tmpWith("BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:x\r\nDTSTART:20260101T090000\r\nDESCRIPTION:"
                "0123456789012345678901234567890123456789012345678901234567890123456789\r\n END:VEVENT\r\nEND:VCALENDAR\r\n");
    obj_scan(KIND_CAL, f, 0, -1, &sc);
    int n = obj_filter(KIND_CAL, f, 0, -1, &sc, view, 64);
    CK(n == 63, "a view that doesn't fit reads as too big");
    fclose(f);
    /* a range of a bigger file, as the batched-download cache is read */
    char two[8192]; snprintf(two, sizeof two, "JUNKJUNK%sTRAILING", EVENT + 0);
    f = tmpWith(two); Appt b; char v2[4096];
    obj_scan(KIND_CAL, f, 8, (long)strlen(EVENT), &sc);
    CK(obj_filter(KIND_CAL, f, 8, (long)strlen(EVENT), &sc, v2, sizeof v2) > 0 && ical_parse(v2, &b) == 0 &&
       !strcmp(b.description, "Team sync"), "read from a range of a file");
    fclose(f);
}

int main(void){
    events();
    contacts();
    phonesAdded();
    staleBase();
    todos();
    odd();
    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
