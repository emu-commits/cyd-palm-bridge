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
