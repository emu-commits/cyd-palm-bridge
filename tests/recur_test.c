/* recur_test.c -- repeating events: which days they fall on, and the iCalendar
 * shapes a ten-year iCloud calendar is full of.
 *
 * Offline. The device used to draw an event only on its start date, so a
 * weekly meeting that began in 2019 never appeared in the days a sync window
 * covers. appt_occurs_on() is what the Day view, the week, the month dots and
 * the lock screen now ask; ical_parse() turns COUNT, "the second Tuesday" and
 * a weekly with no days into what the Palm record can say.
 */
#include <stdio.h>
#include <string.h>
#include "../bridge/palm.h"

static int fails = 0;
static void CK(int c, const char *m){ printf("   %s %s\n", c ? "ok  " : "FAIL", m); if(!c) fails++; }

static Appt ev(const char *rrule_lines){
    char ics[2048];
    snprintf(ics, sizeof ics, "BEGIN:VCALENDAR\r\nBEGIN:VEVENT\r\nUID:r\r\nSUMMARY:x\r\n%sEND:VEVENT\r\nEND:VCALENDAR\r\n", rrule_lines);
    Appt a; ical_parse(ics, &a); return a;
}

int main(void){
    ical_set_tz(NULL);
    printf("== weekly, from 2019 ==\n");
    Appt w = ev("DTSTART:20190107T090000\r\nDTEND:20190107T100000\r\nRRULE:FREQ=WEEKLY;BYDAY=MO\r\n");
    CK(appt_weekday(2019, 1, 7) == 1, "7 Jan 2019 was a Monday");
    CK(appt_occurs_on(&w, 2026, 10, 12) && !appt_occurs_on(&w, 2026, 10, 13), "every Monday, seven years on");
    CK(!appt_occurs_on(&w, 2019, 1, 6) , "never before it began");
    int y, m, d;
    CK(appt_next_on(&w, 2026, 10, 14, 30, &y, &m, &d) && y == 2026 && m == 10 && d == 19, "next one after Wed 14 Oct is Mon 19 Oct");

    printf("== every other week, Tuesday and Thursday ==\n");
    Appt b = ev("DTSTART:20260106T090000\r\nRRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=TU,TH\r\n");
    CK(appt_occurs_on(&b, 2026, 1, 6) && appt_occurs_on(&b, 2026, 1, 8), "the first week, both days");
    CK(!appt_occurs_on(&b, 2026, 1, 13) && !appt_occurs_on(&b, 2026, 1, 15), "not the week after");
    CK(appt_occurs_on(&b, 2026, 1, 20) && appt_occurs_on(&b, 2026, 1, 22), "the week after that");

    printf("== a weekly with no BYDAY: the start's weekday ==\n");
    Appt n = ev("DTSTART:20260105T090000\r\nRRULE:FREQ=WEEKLY\r\n");
    CK(appt_occurs_on(&n, 2026, 1, 12) && !appt_occurs_on(&n, 2026, 1, 13), "Mondays");

    printf("== the second Tuesday, the last Friday ==\n");
    Appt t2 = ev("DTSTART:20260113T180000\r\nRRULE:FREQ=MONTHLY;BYDAY=2TU\r\n");
    CK(t2.repeatType == repeatMonthlyByDay && t2.repeatOn == 1 * 7 + 2, "read as Palm's monthly by day");
    CK(appt_occurs_on(&t2, 2026, 2, 10) && !appt_occurs_on(&t2, 2026, 2, 3) && !appt_occurs_on(&t2, 2026, 2, 17),
       "10 Feb 2026, not the 3rd or the 17th");
    Appt lf = ev("DTSTART:20260130T170000\r\nRRULE:FREQ=MONTHLY;BYDAY=-1FR\r\n");
    CK(lf.repeatOn == 4 * 7 + 5, "the last Friday is Palm's domLastFri");
    CK(appt_occurs_on(&lf, 2026, 2, 27) && !appt_occurs_on(&lf, 2026, 2, 20), "27 Feb 2026, not the 20th");
    char out[2048]; ical_emit(out, sizeof out, &t2, 1);
    CK(strstr(out, "RRULE:FREQ=MONTHLY;BYDAY=2TU") != NULL, "and written back the same way");
    ical_emit(out, sizeof out, &lf, 1);
    CK(strstr(out, "BYDAY=-1FR") != NULL, "the last Friday too");

    printf("== COUNT becomes an end date ==\n");
    Appt c = ev("DTSTART:20260105T090000\r\nRRULE:FREQ=WEEKLY;BYDAY=MO,WE;COUNT=5\r\n");
    CK(!c.repeatForever && c.endYear == 2026 && c.endMonth == 1 && c.endDay == 19, "Mon 5, Wed 7, Mon 12, Wed 14, Mon 19 Jan: ends the 19th");
    CK(appt_occurs_on(&c, 2026, 1, 19) && !appt_occurs_on(&c, 2026, 1, 21), "the fifth and no sixth");

    printf("== exceptions: the latest sixteen of many ==\n");
    char lines[4096] = "DTSTART:20200106T090000\r\nRRULE:FREQ=WEEKLY;BYDAY=MO\r\nEXDATE:";
    int Y = 2020, M = 1, D = 6;
    for(int i = 0; i < 40; i++){
        char one[24]; snprintf(one, sizeof one, "%s%04d%02d%02dT090000", i ? "," : "", Y, M, D);
        strcat(lines, one); appt_add_days(&Y, &M, &D, 7);
    }
    strcat(lines, "\r\n");
    Appt x = ev(lines);
    CK(x.nExcept == 16, "sixteen kept");
    int ly = 2020, lm = 1, ld = 6; appt_add_days(&ly, &lm, &ld, 7 * 39);   /* the 40th, the latest */
    CK(!appt_occurs_on(&x, ly, lm, ld), "the latest exception is among them");
    CK(appt_occurs_on(&x, 2020, 1, 6), "the oldest gave way (a window never shows it)");

    printf("== monthly on the 31st, yearly on 29 Feb ==\n");
    Appt m31 = ev("DTSTART;VALUE=DATE:20260131\r\nRRULE:FREQ=MONTHLY\r\n");
    CK(appt_occurs_on(&m31, 2026, 3, 31) && !appt_occurs_on(&m31, 2026, 2, 28), "skips the short months");
    Appt leap = ev("DTSTART;VALUE=DATE:20240229\r\nRRULE:FREQ=YEARLY\r\n");
    CK(appt_occurs_on(&leap, 2028, 2, 29) && !appt_occurs_on(&leap, 2027, 2, 28), "a leap day, in leap years");

    printf("== a repeat made on the device with no end ==\n");
    {
        /* what the Details sheet used to save: Repeat Daily, the end left as
         * zeros, which packed as 1920-00-00 and so occurred on no day */
        Appt z; memset(&z, 0, sizeof z);
        z.hasTime = 1; z.sH = 9; z.eH = 10; z.year = 2026; z.month = 10; z.day = 15;
        z.hasRepeat = 1; z.repeatType = repeatDaily; z.repeatFreq = 1;
        snprintf(z.description, sizeof z.description, "Daily");
        uint8_t pk[256]; int l = ApptPack(pk, sizeof pk, &z);
        Appt r; CK(l > 0 && ApptUnpack(pk, l, &r) == 0, "it packs and reads back");
        CK(r.repeatForever, "an end with no month or day reads as no end");
        CK(appt_occurs_on(&r, 2026, 10, 15) && appt_occurs_on(&r, 2026, 10, 20), "so it's on its day and every day after");
    }

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
