/* rss.c -- streaming RSS 2.0 / Atom parser + HTML-to-text. See rss.h.
 *
 * The feed is consumed one byte at a time from a source (a file on device, a
 * memory buffer in the host gate), accumulating only the CURRENT <item>/<entry>
 * into a bounded buffer -- so peak RAM is O(one item), not O(feed). Each item's
 * title + richest body is extracted, HTML-stripped, and handed to the callback.
 */
#include "rss.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>

#define ITEM_CAP 8192          /* per-item accumulation cap (larger items truncate) */
#define TEXT_CAP 4096          /* extracted body cap handed to the callback         */
#define TITLE_CAP 256

static char g_item[ITEM_CAP];  /* sync is single-threaded; no reentrancy needed */

/* ---- byte source: file or memory ---- */
typedef struct { const char *mem; int mlen, mpos; FILE *f; } Src;
static int src_getc(Src *s){
    if(s->f) return fgetc(s->f);
    if(s->mem && s->mpos < s->mlen) return (unsigned char)s->mem[s->mpos++];
    return -1;
}

static void put_cp_raw(char *dst, int *n, int cap, unsigned cp);
static int  find_tag(const char *hay, const char *name, char *out, int cap);

/* Decode one UTF-8 sequence at p. Returns bytes consumed (1 for ASCII or for
 * anything malformed, so the scanner always advances) and sets *cp. */
static int utf8_next(const char *p, unsigned *cp){
    const unsigned char *u = (const unsigned char*)p;
    if(u[0] < 0x80){ *cp = u[0]; return 1; }
    int n; unsigned c;
    if     ((u[0] & 0xE0) == 0xC0){ n=2; c = u[0] & 0x1F; }
    else if((u[0] & 0xF0) == 0xE0){ n=3; c = u[0] & 0x0F; }
    else if((u[0] & 0xF8) == 0xF0){ n=4; c = u[0] & 0x07; }
    else { *cp = u[0]; return 1; }                 /* stray continuation byte */
    for(int i=1;i<n;i++){
        if((u[i] & 0xC0) != 0x80){ *cp = u[0]; return 1; }   /* truncated */
        c = (c << 6) | (u[i] & 0x3F);
    }
    *cp = c;
    return n;
}

/* ---- fold what the Palm font cannot draw ---------------------------------
 * lv_font_palm covers U+0020..U+00FF, so accented Latin renders fine but the
 * typographic punctuation every news feed is full of -- curly quotes, en and em
 * dashes, ellipses -- has no glyph and draws as a hollow box. Folding it to the
 * ASCII the font does have is better than either shipping those glyphs (flash we
 * do not have to spare for two faces) or showing boxes. Returns the replacement
 * and its length, or NULL to emit the codepoint unchanged. */
static const char *fold_cp(unsigned cp, int *len){
    const char *r = NULL;
    switch(cp){
    case 0x00A0: r=" ";   break;                       /* nbsp -> a space that wraps */
    case 0x00AD: r="";    break;                       /* soft hyphen: drop          */
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: r="-";  break;
    case 0x2014: case 0x2015: r="--"; break;           /* em dash                    */
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: r="'"; break;
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: r="\""; break;
    case 0x2026: r="..."; break;
    case 0x2039: r="<";   break;
    case 0x203A: r=">";   break;
    case 0x2022: case 0x2023: case 0x25AA: case 0x25CF: r="*"; break;
    case 0x2190: r="<-";  break;
    case 0x2192: r="->";  break;
    case 0x20AC: r="EUR"; break;
    case 0x2122: r="(TM)"; break;
    case 0x2028: case 0x2029: r=" "; break;            /* line/paragraph separator   */
    case 0x200B: case 0x200C: case 0x200D:
    case 0x200E: case 0x200F: case 0xFEFF: r=""; break; /* zero-width / BOM / bidi   */
    default:
        if(cp <= 0xFF) return NULL;                    /* the font has this one      */
        r = "?";                                        /* anything else: one mark    */
        break;
    }
    *len = (int)strlen(r);
    return r;
}

/* append the UTF-8 encoding of a Unicode codepoint to dst[*n], within cap,
 * folding anything the font cannot draw. Runs of the "?" fallback collapse to
 * one, so a headline in a script we cannot render reads as a single mark rather
 * than a wall of them. */
static void put_cp(char *dst, int *n, int cap, unsigned cp){
    int fl = 0;
    const char *f = fold_cp(cp, &fl);
    if(f){
        if(fl == 1 && f[0]=='?' && *n > 0 && dst[*n - 1]=='?') return;   /* collapse */
        for(int i=0;i<fl && *n<cap-1;i++) dst[(*n)++]=f[i];
        return;
    }
    put_cp_raw(dst, n, cap, cp);
}

static void put_cp_raw(char *dst, int *n, int cap, unsigned cp){
    char b[4]; int k=0;
    if(cp < 0x80) b[k++]=(char)cp;
    else if(cp < 0x800){ b[k++]=(char)(0xC0|(cp>>6)); b[k++]=(char)(0x80|(cp&0x3F)); }
    else if(cp < 0x10000){ b[k++]=(char)(0xE0|(cp>>12)); b[k++]=(char)(0x80|((cp>>6)&0x3F)); b[k++]=(char)(0x80|(cp&0x3F)); }
    else { b[k++]=(char)(0xF0|(cp>>18)); b[k++]=(char)(0x80|((cp>>12)&0x3F)); b[k++]=(char)(0x80|((cp>>6)&0x3F)); b[k++]=(char)(0x80|(cp&0x3F)); }
    for(int i=0;i<k && *n<cap-1;i++) dst[(*n)++]=b[i];
}

/* decode a single &entity; starting at s[0]=='&'. writes to dst, advances *n.
 * returns the number of source chars consumed (including & and ;), or 0 if not a
 * recognised entity (caller emits '&' literally). */
static int decode_entity(const char *s, char *dst, int *n, int cap){
    const char *semi = strchr(s, ';');
    if(!semi || semi - s > 12) return 0;
    int len = (int)(semi - s) + 1;
    if(s[1]=='#'){                                   /* numeric */
        unsigned cp=0;
        if(s[2]=='x' || s[2]=='X'){ for(const char *p=s+3;p<semi;p++){ if(!isxdigit((unsigned char)*p)) return 0; cp=cp*16+(isdigit((unsigned char)*p)?*p-'0':(tolower(*p)-'a'+10)); } }
        else { for(const char *p=s+2;p<semi;p++){ if(!isdigit((unsigned char)*p)) return 0; cp=cp*10+(*p-'0'); } }
        if(cp==0 || cp>0x10FFFF) return 0;
        put_cp(dst,n,cap,cp); return len;
    }
    struct { const char *name; unsigned cp; } E[] = {
        {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},
        {"nbsp",' '},{"mdash",0x2014},{"ndash",0x2013},{"hellip",0x2026},
        {"rsquo",0x2019},{"lsquo",0x2018},{"ldquo",0x201C},{"rdquo",0x201D},
    };
    for(unsigned i=0;i<sizeof E/sizeof E[0];i++){
        int L=(int)strlen(E[i].name);
        if(L==len-2 && strncmp(s+1,E[i].name,L)==0){ put_cp(dst,n,cap,E[i].cp); return len; }
    }
    return 0;
}

/* Strip HTML and decode entities in one pass. Crucially, &lt;/&gt; act as tag
 * delimiters too -- RSS <description> commonly carries ENTITY-ESCAPED HTML
 * (&lt;p&gt;...), so decoding first then dropping the tags handles both that and
 * CDATA raw-HTML. sp = a collapsed space is pending. */
int rss_html_to_text(char *dst, int cap, const char *html){
    int n=0, sp=0, in_tag=0;
    for(const char *p=html; *p && n<cap-1; ){
        if(!in_tag && *p=='<'){ in_tag=1; sp=1; p++; continue; }
        if(in_tag){
            if(*p=='>'){ in_tag=0; p++; continue; }
            if(*p=='&'){                                     /* &gt; can close an escaped tag */
                const char *semi=strchr(p,';');
                if(semi && semi-p<=12){
                    if(semi-p==3 && !strncmp(p,"&gt",3)) in_tag=0;
                    p=semi+1; continue;
                }
            }
            p++; continue;                                    /* swallow tag interior */
        }
        if(*p=='&'){
            const char *semi=strchr(p,';');
            if(semi && semi-p==3 && !strncmp(p,"&lt",3)){ in_tag=1; sp=1; p=semi+1; continue; }
            if(sp && n>0 && n<cap-1){ dst[n++]=' '; }
            sp=0;
            int used=decode_entity(p,dst,&n,cap);
            if(used){ p+=used; continue; }
            if(n<cap-1) dst[n++]='&';                         /* not an entity: literal */
            p++; continue;
        }
        if(isspace((unsigned char)*p)){ sp=1; p++; continue; }
        if(sp && n>0 && n<cap-1) dst[n++]=' ';                /* one collapsed space */
        sp=0;
        /* RAW UTF-8, not just entities: a feed writes a curly quote as the three
         * bytes E2 80 99 far more often than as &rsquo;, and copying those bytes
         * through is what put a box on the screen. Decode, then fold. */
        unsigned cp = 0; int seq = utf8_next(p, &cp);
        if(seq > 1){ put_cp(dst,&n,cap,cp); p += seq; continue; }
        if(n<cap-1) dst[n++]=*p;
        p++;
    }
    dst[n]=0;
    return n;
}

/* find <name ...> INNER </name> in hay; copy INNER (CDATA-unwrapped) to out[cap].
 * name-boundary checked so "content" doesn't match "content:encoded". 1 if found. */
static int find_tag(const char *hay, const char *name, char *out, int cap){
    int nl=(int)strlen(name);
    for(const char *p=hay; (p=strchr(p,'<')); p++){
        if(strncmp(p+1,name,nl)!=0) continue;
        char b=p[1+nl];
        if(b!='>' && b!=' ' && b!='\t' && b!='\r' && b!='\n' && b!='/') continue;
        const char *gt=strchr(p+1,'>'); if(!gt) return 0;
        if(gt[-1]=='/') { out[0]=0; return 1; }        /* self-closing, empty */
        const char *inner=gt+1;
        char close[40]; snprintf(close,sizeof close,"</%s>",name);
        const char *ce=strstr(inner,close); if(!ce) return 0;
        const char *s=inner; const char *e=ce;
        if(strncmp(s,"<![CDATA[",9)==0){                /* unwrap CDATA */
            s+=9; const char *cd=strstr(s,"]]>"); if(cd && cd<e) e=cd;
        }
        int len=(int)(e-s); if(len>cap-1) len=cap-1; if(len<0) len=0;
        memcpy(out,s,len); out[len]=0;
        return 1;
    }
    return 0;
}

/* These four were stack locals -- 8.7 KB of frame on the task that also runs the
 * mbedTLS handshake, on a device where the handshake failing for want of ~10 KB
 * is the difference between news and no news. Static for the same reason g_item
 * is: the fetch is single-threaded, one feed at a time. */
static char e_rawt[TITLE_CAP], e_rawb[TEXT_CAP];
static char e_title[TITLE_CAP], e_text[TEXT_CAP];

/* ---- when was this item published? ---------------------------------------
 * Needed so the fetch can keep today's stories and drop last week's. Three
 * shapes cover every feed we ship: RSS 2.0 <pubDate> (RFC 822, "Wed, 20 Aug 2026
 * 14:32:00 GMT"), Atom <published>/<updated> (ISO 8601), and RDF <dc:date>
 * (ISO 8601 too -- Deutsche Welle's feed is RDF). Returns a Unix epoch, or 0 if
 * nothing parsed: an item whose date we cannot read is never assumed to be old.
 * Everything here is UTC arithmetic, so it needs no timezone database. */
static long long days_from_civil(int y, int m, int d){
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d - 1);
    unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

static int month_from_name(const char *s){
    static const char *M = "janfebmaraprmayjunjulaugsepoctnovdec";
    char a = (char)tolower((unsigned char)s[0]);
    char b = (char)tolower((unsigned char)s[1]);
    char c = (char)tolower((unsigned char)s[2]);
    for(int i=0;i<12;i++)
        if(M[i*3]==a && M[i*3+1]==b && M[i*3+2]==c) return i+1;
    return 0;
}

static uint32_t parse_when(const char *s){
    if(!s || !s[0]) return 0;
    while(*s==' '||*s=='\t'||*s=='\r'||*s=='\n') s++;

    int y=0,mo=0,d=0,h=0,mi=0,se=0, off=0;

    if(isdigit((unsigned char)s[0]) && strlen(s) >= 10 && s[4]=='-' && s[7]=='-'){
        /* ISO 8601: 2026-08-20T14:32:00Z / +02:00 / -04:00 */
        if(sscanf(s,"%4d-%2d-%2dT%2d:%2d:%2d",&y,&mo,&d,&h,&mi,&se) < 3) return 0;
        /* the zone suffix starts after the date+time, so scan from index 10 --
         * before that a '-' is a date separator, not an offset sign. */
        for(const char *z = s + 10; *z; z++){
            if(*z=='Z' || *z=='z') break;                       /* UTC */
            if(*z=='+' || *z=='-'){
                int oh=0, om=0;
                if(sscanf(z+1,"%2d:%2d",&oh,&om) < 1) sscanf(z+1,"%2d%2d",&oh,&om);
                off = (*z=='-' ? -1 : 1) * (oh*3600 + om*60);
                break;
            }
        }
    } else {
        /* RFC 822: [Wed, ]20 Aug 2026 14:32:00 GMT|+0200 */
        const char *p = strchr(s, ',');
        p = p ? p+1 : s;
        while(*p==' ') p++;
        char mon[8] = "";
        if(sscanf(p,"%2d %3s %4d %2d:%2d:%2d",&d,mon,&y,&h,&mi,&se) < 3) return 0;
        mo = month_from_name(mon);
        const char *tz = strrchr(p,' ');
        if(tz && (tz[1]=='+' || tz[1]=='-')){
            int v = atoi(tz+1);
            off = (v/100)*3600 + (v%100)*60;
        }
    }
    if(mo < 1 || mo > 12 || d < 1 || d > 31 || y < 1970 || y > 3000) return 0;
    long long t = days_from_civil(y,mo,d) * 86400LL + h*3600 + mi*60 + se - off;
    return t > 0 ? (uint32_t)t : 0;
}

static uint32_t item_when(void){
    char raw[64];
    if(find_tag(g_item,"pubDate",  raw,sizeof raw)) { uint32_t w=parse_when(raw); if(w) return w; }
    if(find_tag(g_item,"published",raw,sizeof raw)) { uint32_t w=parse_when(raw); if(w) return w; }
    if(find_tag(g_item,"updated",  raw,sizeof raw)) { uint32_t w=parse_when(raw); if(w) return w; }
    if(find_tag(g_item,"dc:date",  raw,sizeof raw)) { uint32_t w=parse_when(raw); if(w) return w; }
    if(find_tag(g_item,"date",     raw,sizeof raw)) { uint32_t w=parse_when(raw); if(w) return w; }
    return 0;
}

static void emit_item(int len, int *count, int max_items, rss_item_cb cb, void *ctx){
    (void)len;
    char *rawt = e_rawt, *rawb = e_rawb, *title = e_title, *text = e_text;
    if(!find_tag(g_item,"title",rawt,sizeof e_rawt)) rawt[0]=0;
    /* richest body first: RSS full content, then description, Atom content/summary */
    if(!find_tag(g_item,"content:encoded",rawb,sizeof e_rawb) &&
       !find_tag(g_item,"description",    rawb,sizeof e_rawb) &&
       !find_tag(g_item,"content",        rawb,sizeof e_rawb) &&
       !find_tag(g_item,"summary",        rawb,sizeof e_rawb))
        rawb[0]=0;
    rss_html_to_text(title,sizeof e_title,rawt);
    rss_html_to_text(text, sizeof e_text, rawb);
    if(title[0] || text[0]){ cb(title,text,item_when(),ctx); (*count)++; }
    (void)max_items;
}

/* the shared byte-driven scanner: OUTSIDE looks for <item>/<entry>, INSIDE copies
 * the item body to g_item until the matching close tag. */
static int parse_src(Src *s, int max_items, rss_item_cb cb, void *ctx){
    int count=0, in=0, len=0, c;
    char tag[16]; int tl=0, intag=0;      /* OUT-state open-tag name scanner */
    while((c=src_getc(s))>=0){
        if(!in){
            if(c=='<'){ tl=0; intag=1; }
            else if(intag){
                if(tl<(int)sizeof tag-1 && (isalnum(c)||c==':')) tag[tl++]=(char)c;
                else { tag[tl]=0; intag=0;
                    if(!strcmp(tag,"item")||!strcmp(tag,"entry")){ in=1; len=0; } }
            }
        } else {
            if(len<ITEM_CAP-1) g_item[len++]=(char)c;
            /* close detect: cheap suffix compare */
            if(c=='>' && len>=7){
                if((len>=7 && !memcmp(g_item+len-7,"</item>",7)) ||
                   (len>=8 && !memcmp(g_item+len-8,"</entry>",8))){
                    /* trim the close tag off before extracting */
                    len -= (g_item[len-2]=='m') ? 7 : 8;
                    g_item[len]=0;
                    emit_item(len,&count,max_items,cb,ctx);
                    in=0; len=0;
                    if(max_items>0 && count>=max_items) break;
                }
            }
        }
    }
    return count;
}

int rss_parse_file(const char *path, int max_items, rss_item_cb cb, void *ctx){
    FILE *f=fopen(path,"rb"); if(!f) return -1;
    Src s={0}; s.f=f;
    int n=parse_src(&s,max_items,cb,ctx);
    fclose(f);
    return n;
}
int rss_parse_buf(const char *xml, int len, int max_items, rss_item_cb cb, void *ctx){
    Src s={0}; s.mem=xml; s.mlen=len;
    return parse_src(&s,max_items,cb,ctx);
}
