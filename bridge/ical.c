/* ical.c -- Appt <-> iCalendar VEVENT, with TZID/VTIMEZONE, VALARM, EXDATE,
 * CP1252<->UTF-8 text, and a component-aware parser.
 *
 * Device zone is set once via ical_set_tz(). Timed events emit
 * DTSTART;TZID=<zone>:<wall-clock> (literal, so our own objects round-trip
 * exactly) and the caller ships a matching VTIMEZONE (ical_vtimezone). Foreign
 * UTC inputs (`...Z`) are converted to device-local on parse.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "palm.h"
#include "tz.h"
#include "charset.h"

static const char *BYDAY[7]={"SU","MO","TU","WE","TH","FR","SA"};
static const Tz *g_tz = NULL;

void ical_set_tz(const char *tzid){ g_tz = tz_find(tzid); }
int  ical_vtimezone(char *out,int cap){ return tz_vtimezone(g_tz,out,cap); }

/* ---- RFC5545 text escaping (operates on ASCII control chars only) ---- */
static void esc(char *o,int cap,const char *s){
    int n=0;
    for(;*s && n<cap-2;s++){
        if(*s=='\n'){ o[n++]='\\'; o[n++]='n'; }
        else if(*s=='\\'||*s==';'||*s==','){ o[n++]='\\'; o[n++]=*s; }
        else o[n++]=*s;
    }
    o[n]=0;
}
static void unesc(char *o,int cap,const char *s,int len){
    int n=0;
    for(int i=0;i<len && n<cap-1;i++){
        if(s[i]=='\\' && i+1<len){ char c=s[++i]; o[n++]=(c=='n'||c=='N')?'\n':c; }
        else o[n++]=s[i];
    }
    o[n]=0;
}
/* Palm(CP1252) text -> escaped UTF-8 for output */
static void textOut(char *o,int cap,const char *palm){
    char u8[1024]; cp1252_to_utf8(u8,sizeof u8,palm); esc(o,cap,u8);
}
/* iCal value -> Palm(CP1252) */
static void textIn(char *o,int cap,const char *val,int len){
    char un[1024]; unesc(un,sizeof un,val,len); utf8_to_cp1252(o,cap,un);
}

static int digits(const char *s,int n){ int v=0; for(int i=0;i<n;i++){ if(s[i]<'0'||s[i]>'9')return -1; v=v*10+(s[i]-'0'); } return v; }

int ical_emit(char *out,int cap,const Appt *a,uint32_t uid){
    char desc[1200], note[2200];
    textOut(desc,sizeof desc,a->description);
    textOut(note,sizeof note,a->note);
    const char *tzid = g_tz ? g_tz->id : NULL;

    int n = snprintf(out,cap,"BEGIN:VEVENT\r\nUID:palm-%u@cyd\r\n",uid);
    if(a->hasTime){
        if(tzid){
            n += snprintf(out+n,cap-n,"DTSTART;TZID=%s:%04d%02d%02dT%02d%02d00\r\n",tzid,a->year,a->month,a->day,a->sH,a->sM);
            n += snprintf(out+n,cap-n,"DTEND;TZID=%s:%04d%02d%02dT%02d%02d00\r\n",tzid,a->year,a->month,a->day,a->eH,a->eM);
        } else {
            n += snprintf(out+n,cap-n,"DTSTART:%04d%02d%02dT%02d%02d00\r\n",a->year,a->month,a->day,a->sH,a->sM);
            n += snprintf(out+n,cap-n,"DTEND:%04d%02d%02dT%02d%02d00\r\n",a->year,a->month,a->day,a->eH,a->eM);
        }
    } else {
        n += snprintf(out+n,cap-n,"DTSTART;VALUE=DATE:%04d%02d%02d\r\n",a->year,a->month,a->day);
    }
    if(a->hasRepeat){
        const char *freq=NULL; char extra[64]={0};
        switch(a->repeatType){
            case repeatDaily: freq="DAILY"; break;
            case repeatWeekly: freq="WEEKLY";{
                char b[48]=""; int first=1;
                for(int d=0;d<7;d++) if(a->repeatOn&(1<<d)){ if(!first)strcat(b,","); strcat(b,BYDAY[d]); first=0; }
                if(b[0]) snprintf(extra,sizeof extra,";BYDAY=%s",b);
            } break;
            case repeatMonthlyByDate: case repeatMonthlyByDay: freq="MONTHLY"; break;
            case repeatYearly: freq="YEARLY"; break;
        }
        if(freq){
            n += snprintf(out+n,cap-n,"RRULE:FREQ=%s",freq);
            if(a->repeatFreq>1) n += snprintf(out+n,cap-n,";INTERVAL=%d",a->repeatFreq);
            if(!a->repeatForever) n += snprintf(out+n,cap-n,";UNTIL=%04d%02d%02d",a->endYear,a->endMonth,a->endDay);
            n += snprintf(out+n,cap-n,"%s\r\n",extra);
        }
    }
    /* EXDATE (exception dates) -- match the DTSTART value type */
    if(a->nExcept>0){
        if(a->hasTime && tzid) n += snprintf(out+n,cap-n,"EXDATE;TZID=%s:",tzid);
        else if(a->hasTime)    n += snprintf(out+n,cap-n,"EXDATE:");
        else                   n += snprintf(out+n,cap-n,"EXDATE;VALUE=DATE:");
        for(int i=0;i<a->nExcept;i++){
            if(i) n += snprintf(out+n,cap-n,",");
            if(a->hasTime) n += snprintf(out+n,cap-n,"%04d%02d%02dT%02d%02d00",a->excpt[i].y,a->excpt[i].m,a->excpt[i].d,a->sH,a->sM);
            else           n += snprintf(out+n,cap-n,"%04d%02d%02d",a->excpt[i].y,a->excpt[i].m,a->excpt[i].d);
        }
        n += snprintf(out+n,cap-n,"\r\n");
    }
    if(a->description[0]) n += snprintf(out+n,cap-n,"SUMMARY:%s\r\n",desc);
    if(a->note[0])        n += snprintf(out+n,cap-n,"DESCRIPTION:%s\r\n",note);
    /* VALARM */
    if(a->hasAlarm){
        char unit = a->alarmUnit==2?'D':a->alarmUnit==1?'H':'M';
        n += snprintf(out+n,cap-n,"BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:Reminder\r\n");
        if(unit=='D') n += snprintf(out+n,cap-n,"TRIGGER:-P%dD\r\n",a->alarmAdv);
        else          n += snprintf(out+n,cap-n,"TRIGGER:-PT%d%c\r\n",a->alarmAdv,unit);
        n += snprintf(out+n,cap-n,"END:VALARM\r\n");
    }
    n += snprintf(out+n,cap-n,"END:VEVENT\r\n");
    return n;
}

/* parse ICS date/date-time; if 'Z' suffix, mark utc. */
static void parseDT(const char *v,int *y,int *mo,int *d,int *h,int *mi,int *hasT,int *utc){
    *y=digits(v,4); *mo=digits(v+4,2); *d=digits(v+6,2); *h=0; *mi=0;
    *hasT = (v[8]=='T');
    if(*hasT){ *h=digits(v+9,2); *mi=digits(v+11,2); }
    *utc=0; for(const char*p=v;*p;p++) if(*p=='Z'){ *utc=1; break; }  /* value-only, so a 'Z' means UTC */
}

/* parse a VALARM TRIGGER like -PT10M / -PT2H / -P1D into adv+unit */
static void parseTrigger(const char *v,int *adv,int *unit){
    const char *p=v; if(*p=='-')p++; if(*p=='+')p++;
    if(*p=='P')p++;
    int inTime=0, num=0, have=0;
    for(;*p;p++){
        if(*p=='T'){ inTime=1; continue; }
        if(*p>='0'&&*p<='9'){ num=num*10+(*p-'0'); have=1; }
        else if(have){
            if(*p=='D'){ *adv=num; *unit=2; return; }
            if(*p=='H'){ *adv=num; *unit=1; return; }
            if(*p=='M'&&inTime){ *adv=num; *unit=0; return; }
            num=0; have=0;
        }
    }
}

int ical_parse(const char *ics, Appt *a){
    memset(a,0,sizeof *a);
    int L=(int)strlen(ics);
    char *buf=malloc(L+1); if(!buf) return -1;
    int w=0;
    for(int i=0;i<L;i++){
        if(ics[i]=='\r'||ics[i]=='\n'){
            int j=i; while(j<L && (ics[j]=='\r'||ics[j]=='\n')) j++;
            if(j<L && (ics[j]==' '||ics[j]=='\t')){ i=j; continue; }
            buf[w++]='\n'; i=j-1;
        } else buf[w++]=ics[i];
    }
    buf[w]=0;

    int haveStart=0, inEvent=0, inAlarm=0, inTz=0;
    char *save=NULL;
    for(char *line=strtok_r(buf,"\n",&save); line; line=strtok_r(NULL,"\n",&save)){
        /* component tracking first (BEGIN/END never carry data we want) */
        if(!strncmp(line,"BEGIN:",6)){
            const char*c=line+6;
            if(!strcmp(c,"VEVENT")) inEvent=1;
            else if(!strcmp(c,"VALARM")) inAlarm=1;
            else if(!strcmp(c,"VTIMEZONE")) inTz=1;
            continue;
        }
        if(!strncmp(line,"END:",4)){
            const char*c=line+4;
            if(!strcmp(c,"VEVENT")) inEvent=0;
            else if(!strcmp(c,"VALARM")) inAlarm=0;
            else if(!strcmp(c,"VTIMEZONE")) inTz=0;
            continue;
        }
        if(inTz) continue;                       /* ignore zone rule lines   */

        char *colon=strchr(line,':'); if(!colon) continue;
        *colon=0; char *val=colon+1;
        int isDate=0; char *semi=strchr(line,';');
        if(semi){ if(strstr(semi,"VALUE=DATE")) isDate=1; *semi=0; }
        char *name=line;

        if(inAlarm){
            if(!strcmp(name,"TRIGGER")){ a->hasAlarm=1; a->alarmAdv=0; a->alarmUnit=0; parseTrigger(val,&a->alarmAdv,&a->alarmUnit); }
            continue;
        }
        if(!inEvent) continue;

        if(!strcmp(name,"DTSTART")){
            int y,mo,d,h,mi,hasT,utc; parseDT(val,&y,&mo,&d,&h,&mi,&hasT,&utc);
            if(hasT && !isDate){
                if(utc && g_tz) tz_utc_to_local(g_tz,y,mo,d,h,mi,&y,&mo,&d,&h,&mi);
                a->hasTime=1; a->sH=h; a->sM=mi;
            } else a->hasTime=0;
            a->year=y; a->month=mo; a->day=d; haveStart=1;
        } else if(!strcmp(name,"DTEND")){
            int y,mo,d,h,mi,hasT,utc; parseDT(val,&y,&mo,&d,&h,&mi,&hasT,&utc);
            if(hasT){ if(utc && g_tz) tz_utc_to_local(g_tz,y,mo,d,h,mi,&y,&mo,&d,&h,&mi); a->eH=h; a->eM=mi; }
        } else if(!strcmp(name,"SUMMARY")){
            textIn(a->description,sizeof a->description,val,(int)strlen(val));
        } else if(!strcmp(name,"DESCRIPTION")){
            textIn(a->note,sizeof a->note,val,(int)strlen(val));
        } else if(!strcmp(name,"EXDATE")){
            char *ds=NULL;
            for(char *tok=strtok_r(val,",",&ds); tok && a->nExcept<16; tok=strtok_r(NULL,",",&ds)){
                int y,mo,d,h,mi,hasT,utc; parseDT(tok,&y,&mo,&d,&h,&mi,&hasT,&utc);
                if(hasT && utc && g_tz) tz_utc_to_local(g_tz,y,mo,d,h,mi,&y,&mo,&d,&h,&mi);
                a->excpt[a->nExcept].y=y; a->excpt[a->nExcept].m=mo; a->excpt[a->nExcept].d=d; a->nExcept++;
            }
        } else if(!strcmp(name,"RRULE")){
            a->hasRepeat=1; a->repeatForever=1; a->repeatFreq=1;
            char *rs=NULL;
            for(char *tok=strtok_r(val,";",&rs); tok; tok=strtok_r(NULL,";",&rs)){
                char *eq=strchr(tok,'='); if(!eq) continue; *eq=0; char *rv=eq+1;
                if(!strcmp(tok,"FREQ")){
                    if(!strcmp(rv,"DAILY")) a->repeatType=repeatDaily;
                    else if(!strcmp(rv,"WEEKLY")) a->repeatType=repeatWeekly;
                    else if(!strcmp(rv,"MONTHLY")) a->repeatType=repeatMonthlyByDate;
                    else if(!strcmp(rv,"YEARLY")) a->repeatType=repeatYearly;
                } else if(!strcmp(tok,"INTERVAL")){ a->repeatFreq=atoi(rv);
                } else if(!strcmp(tok,"UNTIL")){
                    int y,mo,d,h,mi,hasT,utc; parseDT(rv,&y,&mo,&d,&h,&mi,&hasT,&utc);
                    a->repeatForever=0; a->endYear=y; a->endMonth=mo; a->endDay=d;
                } else if(!strcmp(tok,"BYDAY")){
                    a->repeatOn=0; char *xs=NULL;
                    for(char *day=strtok_r(rv,",",&xs); day; day=strtok_r(NULL,",",&xs))
                        for(int k=0;k<7;k++) if(!strncmp(day,BYDAY[k],2)) a->repeatOn|=(1<<k);
                }
            }
        }
    }
    if(!a->hasTime){ a->eH=a->sH; a->eM=a->sM; }
    free(buf);
    return haveStart ? 0 : -1;
}
