/* datebook.c -- PalmOS DatebookDB record <-> Appt, both directions.
 *
 * On-disk record (big-endian):
 *   [0..1] startTime  hh mm   (0xFFFF == untimed/all-day)
 *   [2..3] endTime    hh mm
 *   [4..5] date       packed  year(since1904)<<9 | month<<5 | day
 *   [6..7] flags
 *   then, present in flag order: alarm(2), repeat(8), exceptions(2+2n),
 *   description(zstr, even-padded), note(zstr, even-padded).
 *
 * Clean-room from the documented layout. NOTE: the equivalent PumpkinOS
 * ApptUnpack has a bug in the exceptions loop (writes .month twice, never
 * .day) -- fixed here.
 */
#include <string.h>
#include "palm.h"

#define FL_WHEN   0x0001
#define FL_ALARM  0x0002
#define FL_REPEAT 0x0004
#define FL_NOTE   0x0008
#define FL_EXCEP  0x0010
#define FL_DESCR  0x0020

static void unpackDate(uint16_t d,int*y,int*m,int*dd){ *y=(d>>9)+1904; *m=(d>>5)&0x0F; *dd=d&0x1F; }
static uint16_t packDate(int y,int m,int d){ return (uint16_t)(((y-1904)<<9)|((m&0xF)<<5)|(d&0x1F)); }

int ApptUnpack(const uint8_t *r, int len, Appt *a){
    if(len < 8) return -1;
    memset(a,0,sizeof *a);
    a->hasTime = !(r[0]==0xFF && r[1]==0xFF);
    if(a->hasTime){ a->sH=r[0]; a->sM=r[1]; a->eH=r[2]; a->eM=r[3]; }
    unpackDate(be16(r+4), &a->year,&a->month,&a->day);
    uint16_t flags = be16(r+6);

    const uint8_t *w = r + 8;
    int idx = 0;                              /* word index into variable area */
    int words = (len - 8) / 2;
    #define WORD(i) be16(w + (i)*2)
    #define NEED(n) do{ if(idx+(n) > words) return -1; }while(0)

    if(flags & FL_ALARM){ NEED(1); a->hasAlarm=1; a->alarmAdv=r[8+idx*2]; a->alarmUnit=r[8+idx*2+1]; idx+=1; }

    if(flags & FL_REPEAT){
        NEED(4);
        a->hasRepeat = 1;
        a->repeatType = WORD(idx++) >> 8;     /* repeatType in high byte      */
        uint16_t end = WORD(idx++);
        a->repeatForever = (end == 0xFFFF);
        if(!a->repeatForever) unpackDate(end,&a->endYear,&a->endMonth,&a->endDay);
        uint16_t fo = WORD(idx++);
        a->repeatFreq = fo & 0xFF; a->repeatOn = fo >> 8;
        a->startOfWeek = WORD(idx++) & 0xFF;
    }
    if(flags & FL_EXCEP){
        NEED(1);
        int n = WORD(idx++); if(n>16) n=16;
        for(int i=0;i<n;i++){ NEED(1); unpackDate(WORD(idx++), &a->excpt[i].y,&a->excpt[i].m,&a->excpt[i].d); }
        a->nExcept = n;
    }
    if(flags & FL_DESCR){
        const char *s = (const char*)(w + idx*2);
        int max = (int)((r+len) - (const uint8_t*)s);
        int i=0; for(; i<max && i<(int)sizeof a->description-1 && s[i]; i++) a->description[i]=s[i];
        a->description[i]=0;
        int l=i+1; if(l&1) l++; idx += l/2;
    }
    if(flags & FL_NOTE){
        const char *s = (const char*)(w + idx*2);
        int max = (int)((r+len) - (const uint8_t*)s);
        int i=0; for(; i<max && i<(int)sizeof a->note-1 && s[i]; i++) a->note[i]=s[i];
        a->note[i]=0;
    }
    #undef WORD
    #undef NEED
    return 0;
}

int ApptPack(uint8_t *buf, int cap, const Appt *a){
    if(cap < 8) return -1;
    int n = 8;
    if(a->hasTime){ buf[0]=(uint8_t)a->sH; buf[1]=(uint8_t)a->sM; buf[2]=(uint8_t)a->eH; buf[3]=(uint8_t)a->eM; }
    else          { buf[0]=buf[1]=buf[2]=buf[3]=0xFF; }
    put16(buf+4, packDate(a->year,a->month,a->day));
    uint16_t flags = FL_WHEN;

    #define PUT16(v) do{ if(n+2>cap) return -1; put16(buf+n,(uint16_t)(v)); n+=2; }while(0)
    if(a->hasAlarm){ flags|=FL_ALARM; if(n+2>cap) return -1; buf[n]=(uint8_t)a->alarmAdv; buf[n+1]=(uint8_t)a->alarmUnit; n+=2; }
    if(a->hasRepeat){
        flags|=FL_REPEAT;
        PUT16((a->repeatType & 0xFF) << 8);
        PUT16(a->repeatForever ? 0xFFFF : packDate(a->endYear,a->endMonth,a->endDay));
        PUT16(((a->repeatOn & 0xFF) << 8) | (a->repeatFreq & 0xFF));
        PUT16(a->startOfWeek & 0xFF);
    }
    if(a->nExcept > 0){
        flags|=FL_EXCEP;
        PUT16(a->nExcept);
        for(int i=0;i<a->nExcept;i++) PUT16(packDate(a->excpt[i].y,a->excpt[i].m,a->excpt[i].d));
    }
    if(a->description[0]){
        flags|=FL_DESCR;
        int l=(int)strlen(a->description)+1;
        if(n+l>cap) return -1;
        memcpy(buf+n,a->description,l); n+=l;
        if(n&1){ if(n>=cap) return -1; buf[n++]=0; }
    }
    if(a->note[0]){
        flags|=FL_NOTE;
        int l=(int)strlen(a->note)+1;
        if(n+l>cap) return -1;
        memcpy(buf+n,a->note,l); n+=l;
        if(n&1){ if(n>=cap) return -1; buf[n++]=0; }
    }
    #undef PUT16
    put16(buf+6, flags);
    return n;
}

/* ---- occurrences ------------------------------------------------------------
 * Days from 1970-01-01 (H. Hinnant's days_from_civil), so dates compare and
 * subtract as integers, any year. */
static long dfc(int y, int m, int d){
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void civil(long z, int *y, int *m, int *d){
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2) / 153;
    *d = (int)(doy - (153*mp + 2)/5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)((long)yoe + era * 400 + (*m <= 2));
}
void appt_add_days(int *y, int *m, int *d, int n){ civil(dfc(*y, *m, *d) + n, y, m, d); }
int appt_weekday(int y, int m, int d){ long z = dfc(y, m, d); return (int)(((z % 7) + 7 + 4) % 7); }
static int dim(int y, int m){
    static const int n[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if(m < 1 || m > 12) return 31;
    return n[m-1] + (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
}
int appt_occurs_on(const Appt *a, int y, int m, int d){
    if(a->year < 1904 || a->month < 1 || a->day < 1) return 0;
    long t = dfc(y, m, d), s = dfc(a->year, a->month, a->day);
    if(t < s) return 0;
    if(!a->hasRepeat || a->repeatType == repeatNone) return t == s;
    if(!a->repeatForever && a->endYear >= 1904 && t > dfc(a->endYear, a->endMonth, a->endDay)) return 0;
    for(int i = 0; i < a->nExcept && i < 16; i++)
        if(a->excpt[i].y == y && a->excpt[i].m == m && a->excpt[i].d == d) return 0;
    int f = a->repeatFreq > 0 ? a->repeatFreq : 1;
    int months = (y - a->year) * 12 + (m - a->month);
    switch(a->repeatType){
    case repeatDaily:
        return (t - s) % f == 0;
    case repeatWeekly: {
        int wd = appt_weekday(y, m, d);
        int on = a->repeatOn & 0x7F;
        if(!on) on = 1 << appt_weekday(a->year, a->month, a->day);   /* no days given: the start's */
        if(!(on & (1 << wd))) return 0;
        /* every f weeks, counted in weeks that begin on startOfWeek */
        int sow = a->startOfWeek % 7;
        long ws = s - ((appt_weekday(a->year, a->month, a->day) - sow + 7) % 7);
        long wt = t - ((wd - sow + 7) % 7);
        return ((wt - ws) / 7) % f == 0;
    }
    case repeatMonthlyByDate:
        return months % f == 0 && d == a->day;          /* a 31st skips the short months */
    case repeatMonthlyByDay: {
        if(months % f) return 0;
        int week = a->repeatOn / 7, dow = a->repeatOn % 7;   /* Palm: dom1stSun=0 .. domLastSat=34 */
        if(appt_weekday(y, m, d) != dow) return 0;
        return week < 4 ? (d - 1) / 7 == week : d + 7 > dim(y, m);
    }
    case repeatYearly:
        return (y - a->year) % f == 0 && m == a->month && d == a->day;
    }
    return t == s;
}
int appt_next_on(const Appt *a, int y, int m, int d, int horizon, int *oy, int *om, int *od){
    long t = dfc(y, m, d);
    long s = a->year >= 1904 ? dfc(a->year, a->month, a->day) : t;
    if(t < s) t = s;                                    /* nothing before the start */
    for(int i = 0; i <= horizon; i++, t++){
        int yy, mm, dd; civil(t, &yy, &mm, &dd);
        if(appt_occurs_on(a, yy, mm, dd)){ *oy = yy; *om = mm; *od = dd; return 1; }
        if(!a->hasRepeat && t > s) return 0;
    }
    return 0;
}
