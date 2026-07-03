/* tz.c -- timezone registry + DST math + VTIMEZONE emission. */
#include <stdio.h>
#include <string.h>
#include "tz.h"

static const Tz ZONES[] = {
    { "America/New_York",    -300, -240, DST_US, "EST", "EDT" },
    { "America/Chicago",     -360, -300, DST_US, "CST", "CDT" },
    { "America/Denver",      -420, -360, DST_US, "MST", "MDT" },
    { "America/Los_Angeles", -480, -420, DST_US, "PST", "PDT" },
    { "America/Phoenix",     -420, -420, DST_NONE,"MST", "MST" },
    { "Europe/London",          0,   60, DST_EU, "GMT", "BST" },
    { "Europe/Paris",          60,  120, DST_EU, "CET", "CEST" },
    { "Europe/Berlin",         60,  120, DST_EU, "CET", "CEST" },
    { "UTC",                    0,    0, DST_NONE,"UTC", "UTC" },
};

const Tz *tz_find(const char *id){
    if(!id||!id[0]) return NULL;
    for(size_t i=0;i<sizeof ZONES/sizeof ZONES[0];i++)
        if(!strcmp(ZONES[i].id,id)) return &ZONES[i];
    return NULL;
}

/* ---- civil-date arithmetic (Howard Hinnant's algorithms) ---- */
static long days_from_civil(int y,int m,int d){
    y -= m <= 2;
    long era = (y>=0?y:y-399)/400;
    unsigned yoe = (unsigned)(y - era*400);
    unsigned doy = (153*(m + (m>2?-3:9)) + 2)/5 + d-1;
    unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
    return era*146097 + (long)doe - 719468;
}
static void civil_from_days(long z,int*y,int*m,int*d){
    z += 719468;
    long era = (z>=0?z:z-146096)/146097;
    unsigned doe = (unsigned)(z - era*146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096)/365;
    int yy = (int)(yoe) + (int)era*400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2)/153;
    *d = (int)(doy - (153*mp+2)/5 + 1);
    *m = (int)(mp + (mp<10?3:-9));
    *y = yy + (*m<=2);
}
static long lin(int y,int mo,int d,int h,int mi){ return days_from_civil(y,mo,d)*1440L + h*60 + mi; }
static int weekday(int y,int m,int d){ long z=days_from_civil(y,m,d); int w=(int)((z%7+4+7)%7); return w; } /* 0=Sun */
static int nthSunday(int y,int m,int n){ int w=weekday(y,m,1); int first=1+((7-w)%7); return first+(n-1)*7; }
static int lastSunday(int y,int m){
    static const int dim[]={31,28,31,30,31,30,31,31,30,31,30,31};
    int last=dim[m-1]; if(m==2 && ((y%4==0&&y%100!=0)||y%400==0)) last=29;
    int w=weekday(y,m,last); return last-w; /* w days back to Sunday */
}

/* offset (minutes east) that applies at the given UTC instant */
int tz_offset_utc(const Tz*z,int y,int mo,int d,int h,int mi){
    if(!z||z->rule==DST_NONE) return z?z->stdOff:0;
    long t = lin(y,mo,d,h,mi);
    long springUTC, fallUTC;
    if(z->rule==DST_US){
        /* 2nd Sun Mar 02:00 local-standard ; 1st Sun Nov 02:00 local-daylight */
        springUTC = lin(y,3,nthSunday(y,3,2),0,0) + (120 - z->stdOff);
        fallUTC   = lin(y,11,nthSunday(y,11,1),0,0) + (120 - z->dstOff);
    } else { /* EU: last Sun Mar 01:00 UTC ; last Sun Oct 01:00 UTC */
        springUTC = lin(y,3,lastSunday(y,3),1,0);
        fallUTC   = lin(y,10,lastSunday(y,10),1,0);
    }
    return (t>=springUTC && t<fallUTC) ? z->dstOff : z->stdOff;
}

void tz_utc_to_local(const Tz*z,int y,int mo,int d,int h,int mi,
                     int*oy,int*omo,int*od,int*oh,int*omi){
    int off = z ? tz_offset_utc(z,y,mo,d,h,mi) : 0;
    long lm = lin(y,mo,d,h,mi) + off;
    long days = lm/1440; long rem = lm%1440; if(rem<0){ rem+=1440; days--; }
    civil_from_days(days,oy,omo,od);
    *oh=(int)(rem/60); *omi=(int)(rem%60);
}

static void fmtOff(int m,char*out){ char s=m<0?'-':'+'; int a=(m<0?-m:m)%(24*60); snprintf(out,8,"%c%02d%02d",s,a/60,a%60); }

int tz_vtimezone(const Tz*z,char*out,int cap){
    if(!z || z->rule==DST_NONE){
        if(!z) return (out&&cap)?(out[0]=0,0):0;
        char so[8]; fmtOff(z->stdOff,so);
        return snprintf(out,cap,
            "BEGIN:VTIMEZONE\r\nTZID:%s\r\nBEGIN:STANDARD\r\nDTSTART:19700101T000000\r\n"
            "TZOFFSETFROM:%s\r\nTZOFFSETTO:%s\r\nTZNAME:%s\r\nEND:STANDARD\r\nEND:VTIMEZONE\r\n",
            z->id,so,so,z->stdName);
    }
    char sf[8],st[8]; fmtOff(z->stdOff,sf); fmtOff(z->dstOff,st);
    int dMonth = (z->rule==DST_US)?3:3, sMonth=(z->rule==DST_US)?11:10;
    const char*dRule = (z->rule==DST_US)?"FREQ=YEARLY;BYMONTH=3;BYDAY=2SU":"FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU";
    const char*sRule = (z->rule==DST_US)?"FREQ=YEARLY;BYMONTH=11;BYDAY=1SU":"FREQ=YEARLY;BYMONTH=10;BYDAY=-1SU";
    const char*dTime = (z->rule==DST_US)?"020000":"010000";
    const char*sTime = (z->rule==DST_US)?"020000":"010000";
    (void)dMonth;(void)sMonth;
    return snprintf(out,cap,
        "BEGIN:VTIMEZONE\r\nTZID:%s\r\n"
        "BEGIN:DAYLIGHT\r\nDTSTART:19700308T%s\r\nTZOFFSETFROM:%s\r\nTZOFFSETTO:%s\r\nTZNAME:%s\r\nRRULE:%s\r\nEND:DAYLIGHT\r\n"
        "BEGIN:STANDARD\r\nDTSTART:19701101T%s\r\nTZOFFSETFROM:%s\r\nTZOFFSETTO:%s\r\nTZNAME:%s\r\nRRULE:%s\r\nEND:STANDARD\r\n"
        "END:VTIMEZONE\r\n",
        z->id,
        dTime,sf,st,z->dstName,dRule,
        sTime,st,sf,z->stdName,sRule);
}
