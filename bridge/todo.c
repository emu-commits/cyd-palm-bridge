/* todo.c -- PalmOS ToDoDB record <-> Todo <-> iCalendar VTODO.
 *
 * On-disk record (big-endian):
 *   [0..1] dueDate  packed (year-1904)<<9|month<<5|day  (0xFFFF = no due date)
 *   [2]    priority: bit7 = completed flag, low bits = priority (1..5)
 *   [3..]  description (zstr), then note (zstr)
 *
 * VTODO maps: SUMMARY/DESCRIPTION, DUE;VALUE=DATE, PRIORITY (Palm 1..5 <-> iCal
 * 1/3/5/7/9), STATUS + PERCENT-COMPLETE for the completed flag. Text is
 * CP1252<->UTF-8 like the calendar codec.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "palm.h"
#include "charset.h"

static void unpackDate(uint16_t d,int*y,int*m,int*dd){ *y=(d>>9)+1904; *m=(d>>5)&0x0F; *dd=d&0x1F; }
static uint16_t packDate(int y,int m,int d){ return (uint16_t)(((y-1904)<<9)|((m&0xF)<<5)|(d&0x1F)); }

int ToDoUnpack(const uint8_t *r, int len, Todo *t){
    if(len < 3) return -1;
    memset(t,0,sizeof *t);
    uint16_t due = be16(r);
    t->hasDue = (due != 0xFFFF);
    if(t->hasDue) unpackDate(due,&t->dueY,&t->dueM,&t->dueD);
    t->completed = (r[2] & 0x80) ? 1 : 0;
    t->priority  = r[2] & 0x7F;
    const char *p = (const char*)(r + 3);
    const char *end = (const char*)(r + len);
    int i=0; for(; p<end && *p && i<(int)sizeof t->description-1; i++,p++) t->description[i]=*p;
    t->description[i]=0; if(p<end) p++;               /* skip NUL */
    i=0; for(; p<end && *p && i<(int)sizeof t->note-1; i++,p++) t->note[i]=*p;
    t->note[i]=0;
    return 0;
}

int ToDoPack(uint8_t *buf, int cap, const Todo *t){
    if(cap < 3) return -1;
    put16(buf, t->hasDue ? packDate(t->dueY,t->dueM,t->dueD) : 0xFFFF);
    buf[2] = (uint8_t)((t->completed?0x80:0) | (t->priority & 0x7F));
    int n=3;
    int dl=(int)strlen(t->description)+1; if(n+dl>cap) return -1; memcpy(buf+n,t->description,dl); n+=dl;
    int nl=(int)strlen(t->note)+1;        if(n+nl>cap) return -1; memcpy(buf+n,t->note,nl);        n+=nl;
    return n;
}

/* ---- small text escaping (same rules as ical.c) ---- */
static void esc(char*o,int cap,const char*s){ int n=0;
    for(;*s&&n<cap-2;s++){ if(*s=='\n'){o[n++]='\\';o[n++]='n';}
        else if(*s=='\\'||*s==';'||*s==','){o[n++]='\\';o[n++]=*s;} else o[n++]=*s; } o[n]=0; }
static void unesc(char*o,int cap,const char*s,int len){ int n=0;
    for(int i=0;i<len&&n<cap-1;i++){ if(s[i]=='\\'&&i+1<len){char c=s[++i];o[n++]=(c=='n'||c=='N')?'\n':c;} else o[n++]=s[i]; } o[n]=0; }
static void textOut(char*o,int cap,const char*palm){ char u8[1024]; cp1252_to_utf8(u8,sizeof u8,palm); esc(o,cap,u8); }
static void textIn(char*o,int cap,const char*v,int len){ char un[1024]; unesc(un,sizeof un,v,len); utf8_to_cp1252(o,cap,un); }

/* Palm priority 1..5 <-> iCal 1/3/5/7/9 */
static int palmToIcal(int p){ if(p<1)return 0; if(p>5)p=5; return 2*p-1; }
static int icalToPalm(int p){ if(p<=0)return 1; if(p<=1)return 1; if(p<=3)return 2; if(p<=5)return 3; if(p<=7)return 4; return 5; }

int vtodo_emit(char *out,int cap,const Todo *t,uint32_t uid){
    char desc[600], note[1100];
    textOut(desc,sizeof desc,t->description);
    textOut(note,sizeof note,t->note);
    int n = snprintf(out,cap,"BEGIN:VTODO\r\nUID:palm-%u@cyd\r\n",(unsigned)uid);
    if(t->description[0]) n += snprintf(out+n,cap-n,"SUMMARY:%s\r\n",desc);
    if(t->note[0])        n += snprintf(out+n,cap-n,"DESCRIPTION:%s\r\n",note);
    if(t->hasDue)         n += snprintf(out+n,cap-n,"DUE;VALUE=DATE:%04d%02d%02d\r\n",t->dueY,t->dueM,t->dueD);
    if(t->priority>=1)    n += snprintf(out+n,cap-n,"PRIORITY:%d\r\n",palmToIcal(t->priority));
    if(t->completed) n += snprintf(out+n,cap-n,"STATUS:COMPLETED\r\nPERCENT-COMPLETE:100\r\n");
    else             n += snprintf(out+n,cap-n,"STATUS:NEEDS-ACTION\r\n");
    n += snprintf(out+n,cap-n,"END:VTODO\r\n");
    return n;
}

static int digits(const char*s,int n){ int v=0; for(int i=0;i<n;i++){ if(s[i]<'0'||s[i]>'9')return -1; v=v*10+(s[i]-'0'); } return v; }

int vtodo_parse(const char *ics, Todo *t){
    memset(t,0,sizeof *t);
    int L=(int)strlen(ics); char *buf=malloc(L+1); if(!buf) return -1;
    int w=0;
    for(int i=0;i<L;i++){
        if(ics[i]=='\r'||ics[i]=='\n'){ int j=i; while(j<L&&(ics[j]=='\r'||ics[j]=='\n'))j++;
            if(j<L&&(ics[j]==' '||ics[j]=='\t')){ i=j; continue; } buf[w++]='\n'; i=j-1; }
        else buf[w++]=ics[i];
    }
    buf[w]=0;
    int have=0, inTodo=0;
    char *save=NULL;
    for(char*line=strtok_r(buf,"\n",&save); line; line=strtok_r(NULL,"\n",&save)){
        if(!strncmp(line,"BEGIN:",6)){ if(!strcmp(line+6,"VTODO")) inTodo=1; continue; }
        if(!strncmp(line,"END:",4)){ if(!strcmp(line+4,"VTODO")) inTodo=0; continue; }
        if(!inTodo) continue;
        char*colon=strchr(line,':'); if(!colon) continue; *colon=0; char*val=colon+1;
        char*semi=strchr(line,';'); if(semi)*semi=0; char*name=line;
        if(!strcmp(name,"SUMMARY")){ textIn(t->description,sizeof t->description,val,(int)strlen(val)); have=1; }
        else if(!strcmp(name,"DESCRIPTION")){ textIn(t->note,sizeof t->note,val,(int)strlen(val)); }
        else if(!strcmp(name,"DUE")){ int L=(int)strlen(val); t->hasDue=1;   /* length-guarded: don't read past a short/truncated value */
            t->dueY=L>=4?digits(val,4):-1; t->dueM=L>=6?digits(val+4,2):-1; t->dueD=L>=8?digits(val+6,2):-1; have=1; }
        else if(!strcmp(name,"PRIORITY")){ t->priority=icalToPalm(atoi(val)); }
        else if(!strcmp(name,"STATUS")){ if(!strcmp(val,"COMPLETED")) t->completed=1; }
        else if(!strcmp(name,"PERCENT-COMPLETE")){ if(atoi(val)>=100) t->completed=1; }
    }
    free(buf);
    return have ? 0 : -1;
}
