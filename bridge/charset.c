/* charset.c -- CP1252 <-> UTF-8. */
#include "charset.h"

/* CP1252 0x80..0x9F -> Unicode codepoint (0 = undefined in CP1252). */
static const unsigned CP1252_HI[32] = {
 0x20AC,0,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,0x02C6,0x2030,0x0160,0x2039,0x0152,0,0x017D,0,
 0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x02DC,0x2122,0x0161,0x203A,0x0153,0,0x017E,0x0178
};

static int enc(unsigned cp,char*o,int cap){
    if(cp<0x80){ if(cap<1)return 0; o[0]=(char)cp; return 1; }
    if(cp<0x800){ if(cap<2)return 0; o[0]=(char)(0xC0|(cp>>6)); o[1]=(char)(0x80|(cp&0x3F)); return 2; }
    if(cap<3)return 0;
    o[0]=(char)(0xE0|(cp>>12)); o[1]=(char)(0x80|((cp>>6)&0x3F)); o[2]=(char)(0x80|(cp&0x3F)); return 3;
}

int cp1252_to_utf8(char *dst,int cap,const char *src){
    int n=0;
    for(const unsigned char*s=(const unsigned char*)src; *s; s++){
        unsigned cp;
        if(*s<0x80) cp=*s;
        else if(*s<0xA0){ cp=CP1252_HI[*s-0x80]; if(!cp) cp='?'; }
        else cp=*s;                 /* 0xA0..0xFF == Latin-1 == same codepoint */
        int k=enc(cp,dst+n,cap-1-n); if(k==0) break; n+=k;
    }
    dst[n]=0; return n;
}

int utf8_to_cp1252(char *dst,int cap,const char *src){
    int n=0;
    const unsigned char*s=(const unsigned char*)src;
    while(*s && n<cap-1){
        unsigned cp; int k;
        if(*s<0x80){ cp=*s; k=1; }
        else if((*s>>5)==0x6){ cp=((*s&0x1F)<<6)|(s[1]&0x3F); k=2; if(!(s[1]&0x80))k=1; }
        else if((*s>>4)==0xE){ cp=((*s&0x0F)<<12)|((s[1]&0x3F)<<6)|(s[2]&0x3F); k=3; }
        else { cp='?'; k=1; }
        s+=k;
        unsigned char b;
        if(cp<0x80||(cp>=0xA0&&cp<=0xFF)) b=(unsigned char)cp;
        else { b='?'; for(int i=0;i<32;i++) if(CP1252_HI[i]==cp){ b=(unsigned char)(0x80+i); break; } }
        dst[n++]=(char)b;
    }
    dst[n]=0; return n;
}
