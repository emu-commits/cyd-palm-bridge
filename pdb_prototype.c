/* pdb_prototype.c -- de-risk spike for CYD PalmOS->CalDAV bridge.
 *
 * Proves the plan's core seam WITHOUT PumpkinOS's DataMgr/storage/MemHandle
 * runtime and WITHOUT ever holding the database in RAM:
 *
 *   raw .pdb file  --(tiny container reader)-->  raw record bytes
 *                  --(ApptUnpack port)--------->  ApptDBRecord struct
 *                  --(emitter)----------------->  iCalendar VEVENT
 *
 * ApptUnpack logic ported from PumpkinOS src/DateBook/DateDB.c (orig Palm SDK,
 * Roger Flores 1/25/95), made endianness-correct (original cast a big-endian
 * struct; we read BE explicitly so it runs on the little-endian ESP32/host).
 *
 * Build:  cc -Wall -O2 -o pdb_prototype pdb_prototype.c
 * Run:    ./pdb_prototype            (self-generates a sample DatebookDB.pdb)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- Palm flag bits (from DateDB.h) ---- */
#define APPT_FLAG_WHEN   0x01
#define APPT_FLAG_ALARM  0x02
#define APPT_FLAG_REPEAT 0x04
#define APPT_FLAG_NOTE   0x08
#define APPT_FLAG_EXCEP  0x10
#define APPT_FLAG_DESCR  0x20

/* RepeatType enum (DateDB.h) */
enum { repeatNone, repeatDaily, repeatWeekly, repeatMonthlyByDay,
       repeatMonthlyByDate, repeatYearly };

/* ---- unpacked view (subset of ApptDBRecordType we care about) ---- */
typedef struct {
    int   hasTime;          /* 0 == untimed/all-day */
    int   sH, sM, eH, eM;   /* start/end hour:min   */
    int   year, month, day; /* Gregorian            */
    int   hasRepeat;
    int   repeatType, repeatFreq, repeatOn, repeatForever;
    int   endYear, endMonth, endDay;
    char  description[256];
    char  note[512];
} Appt;

/* ---- big-endian readers over a flat byte buffer (this is the whole
 *      "no DataMgr, no MemHandle" story: records are just bytes) ---- */
static uint16_t be16(const uint8_t *p){ return (uint16_t)((p[0]<<8)|p[1]); }
static uint32_t be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3]; }

/* PalmOS DateType is packed into 16 bits: year(7, since 1904)|month(4)|day(5) */
static void unpackDate(uint16_t d,int*y,int*m,int*dd){ *y=(d>>9)+1904; *m=(d>>5)&0x0F; *dd=d&0x1F; }

/* ---- the port: raw packed record bytes -> Appt ---- */
static void ApptUnpack(const uint8_t *r, int len, Appt *a){
    memset(a,0,sizeof *a);
    /* fixed 8-byte head: startTime(2) endTime(2) date(2) flags(2) */
    a->hasTime = !(r[0]==0xFF && r[1]==0xFF);
    a->sH=r[0]; a->sM=r[1]; a->eH=r[2]; a->eM=r[3];
    unpackDate(be16(r+4), &a->year,&a->month,&a->day);
    uint16_t flags = be16(r+6);

    const uint8_t *w = r + 8;          /* variable area, word-indexed */
    int idx = 0;                       /* index in 16-bit words       */
    #define WORD(i) be16(w + (i)*2)

    if (flags & APPT_FLAG_ALARM) idx += 1;                 /* skip alarm word */

    if (flags & APPT_FLAG_REPEAT){
        a->hasRepeat = 1;
        a->repeatType = WORD(idx++) & 0xFF;
        uint16_t end = WORD(idx++);
        a->repeatForever = (end == 0xFFFF);                /* 0x7f/0x0f/0x1f sentinel */
        unpackDate(end,&a->endYear,&a->endMonth,&a->endDay);
        uint16_t fo = WORD(idx++);
        a->repeatFreq = fo & 0xFF; a->repeatOn = fo >> 8;
        idx++;                                              /* repeatStartOfWeek word */
    }
    if (flags & APPT_FLAG_EXCEP){
        int n = WORD(idx++); if(n>16)n=16; idx += n;        /* skip exception dates */
    }
    if (flags & APPT_FLAG_DESCR){
        const char *s = (const char*)(w + idx*2);
        strncpy(a->description, s, sizeof a->description -1);
        int l = (int)strlen(s)+1; if(l&1)l++; idx += l>>1;
    }
    if (flags & APPT_FLAG_NOTE){
        const char *s = (const char*)(w + idx*2);
        strncpy(a->note, s, sizeof a->note -1);
    }
    (void)len; (void)be32;
    #undef WORD
}

/* ---- iCalendar emitter ---- */
static const char *BYDAY[7]={"SU","MO","TU","WE","TH","FR","SA"};
static void emitVEVENT(const Appt*a,int uid){
    printf("BEGIN:VEVENT\r\nUID:palm-%d@cyd\r\n",uid);
    if(a->hasTime){
        printf("DTSTART:%04d%02d%02dT%02d%02d00\r\n",a->year,a->month,a->day,a->sH,a->sM);
        printf("DTEND:%04d%02d%02dT%02d%02d00\r\n",a->year,a->month,a->day,a->eH,a->eM);
    } else {
        printf("DTSTART;VALUE=DATE:%04d%02d%02d\r\n",a->year,a->month,a->day);
    }
    if(a->hasRepeat){
        const char*freq=0; char extra[64]={0};
        switch(a->repeatType){
            case repeatDaily: freq="DAILY"; break;
            case repeatWeekly: freq="WEEKLY";{
                char b[64]=""; int first=1;
                for(int d=0;d<7;d++) if(a->repeatOn&(1<<d)){
                    if(!first)strcat(b,","); strcat(b,BYDAY[d]); first=0; }
                if(b[0]) snprintf(extra,sizeof extra,";BYDAY=%s",b);
            } break;
            case repeatMonthlyByDate: freq="MONTHLY"; break;
            case repeatMonthlyByDay:  freq="MONTHLY"; break;
            case repeatYearly: freq="YEARLY"; break;
        }
        if(freq){
            printf("RRULE:FREQ=%s",freq);
            if(a->repeatFreq>1) printf(";INTERVAL=%d",a->repeatFreq);
            if(!a->repeatForever) printf(";UNTIL=%04d%02d%02d",a->endYear,a->endMonth,a->endDay);
            printf("%s\r\n",extra);
        }
    }
    if(a->description[0]) printf("SUMMARY:%s\r\n",a->description);
    if(a->note[0])        printf("DESCRIPTION:%s\r\n",a->note);
    printf("END:VEVENT\r\n");
}

/* ================= sample-data generator (packs a real DatebookDB.pdb) ===== */
static void put16(uint8_t*p,uint16_t v){p[0]=v>>8;p[1]=v&0xFF;}
static void put32(uint8_t*p,uint32_t v){p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v;}
static uint16_t packDate(int y,int m,int d){return (uint16_t)(((y-1904)<<9)|(m<<5)|d);}

/* build one packed record into buf, return length */
static int packAppt(uint8_t*buf,int sH,int sM,int eH,int eM,int y,int m,int d,
                    int rptType,int rptFreq,int rptOn,const char*desc,const char*note){
    int flags=APPT_FLAG_WHEN, n=8;
    if(sH<0){buf[0]=0xFF;buf[1]=0xFF;buf[2]=0xFF;buf[3]=0xFF;}
    else    {buf[0]=sH;buf[1]=sM;buf[2]=eH;buf[3]=eM;}
    put16(buf+4,packDate(y,m,d));
    if(rptType!=repeatNone){
        flags|=APPT_FLAG_REPEAT;
        put16(buf+n,rptType&0xFF); n+=2;
        put16(buf+n,0xFFFF); n+=2;                 /* end = forever */
        put16(buf+n,(rptOn<<8)|(rptFreq&0xFF)); n+=2;
        put16(buf+n,0); n+=2;                       /* startOfWeek */
    }
    if(desc){flags|=APPT_FLAG_DESCR; int l=strlen(desc)+1; strcpy((char*)buf+n,desc); n+=l; if(n&1)buf[n++]=0;}
    if(note){flags|=APPT_FLAG_NOTE;  int l=strlen(note)+1; strcpy((char*)buf+n,note); n+=l; if(n&1)buf[n++]=0;}
    put16(buf+6,flags);
    return n;
}

static void writeSamplePDB(const char*path){
    uint8_t rec[3][512]; int rlen[3];
    rlen[0]=packAppt(rec[0],14,0,15,0, 2026,7,2, repeatNone,0,0, "Dentist appointment","Bring insurance card");
    rlen[1]=packAppt(rec[1],-1,0,0,0,  2026,7,4, repeatNone,0,0, "Independence Day",NULL);            /* all-day */
    rlen[2]=packAppt(rec[2],9,30,10,0, 2026,7,6, repeatWeekly,1,(1<<1)|(1<<3),"Team standup",NULL);   /* Mon+Wed */

    int nrec=3, hdr=78, idx=8*nrec+2; /* +2 pad */
    uint8_t H[78]; memset(H,0,sizeof H);
    memcpy(H,"DatebookDB",10);
    put32(H+0x20,0); /* dates */ put32(H+0x3C,0x44415441); /* 'DATA' */
    put32(H+0x40,0x64617465); /* 'date' creator */
    put16(H+0x4C,nrec);
    FILE*f=fopen(path,"wb"); fwrite(H,1,78,f);
    int off=hdr+idx; uint8_t e[8];
    for(int i=0;i<nrec;i++){ put32(e,off); e[4]=0; e[5]=e[6]=e[7]=0; e[7]=i+1; fwrite(e,1,8,f); off+=rlen[i]; }
    uint8_t pad[2]={0,0}; fwrite(pad,1,2,f);
    for(int i=0;i<nrec;i++) fwrite(rec[i],1,rlen[i],f);
    fclose(f);
}

/* ================= the actual "no DB in RAM" reader loop ================== */
static void streamPDB(const char*path){
    FILE*f=fopen(path,"rb"); if(!f){perror(path);exit(1);}
    uint8_t H[78]; fread(H,1,78,f);
    int nrec=be16(H+0x4C);
    /* read the record index (8 bytes each) into small arrays -- offsets only */
    uint32_t *off=malloc(sizeof(uint32_t)*(nrec+1));
    for(int i=0;i<nrec;i++){uint8_t e[8]; fread(e,1,8,f); off[i]=be32(e);}
    fseek(f,0,SEEK_END); off[nrec]=ftell(f);

    printf("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//CYD-Palm-Bridge//EN\r\n");
    uint8_t buf[1024];                       /* ONE record at a time <-- RAM proof */
    for(int i=0;i<nrec;i++){
        int len=off[i+1]-off[i]; if(len>(int)sizeof buf)len=sizeof buf;
        fseek(f,off[i],SEEK_SET); fread(buf,1,len,f);
        Appt a; ApptUnpack(buf,len,&a);
        emitVEVENT(&a,i+1);
    }
    printf("END:VCALENDAR\r\n");
    free(off); fclose(f);
    fprintf(stderr,"[peak record buffer: %zu bytes for %d records]\n",sizeof buf,nrec);
}

int main(void){
    const char*p="DatebookDB.pdb";
    writeSamplePDB(p);
    streamPDB(p);
    return 0;
}
