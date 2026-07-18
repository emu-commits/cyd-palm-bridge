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

/* append the UTF-8 encoding of a Unicode codepoint to dst[*n], within cap */
static void put_cp(char *dst, int *n, int cap, unsigned cp){
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

static void emit_item(int len, int *count, int max_items, rss_item_cb cb, void *ctx){
    (void)len;
    char rawt[TITLE_CAP], rawb[TEXT_CAP];
    char title[TITLE_CAP], text[TEXT_CAP];
    if(!find_tag(g_item,"title",rawt,sizeof rawt)) rawt[0]=0;
    /* richest body first: RSS full content, then description, Atom content/summary */
    if(!find_tag(g_item,"content:encoded",rawb,sizeof rawb) &&
       !find_tag(g_item,"description",    rawb,sizeof rawb) &&
       !find_tag(g_item,"content",        rawb,sizeof rawb) &&
       !find_tag(g_item,"summary",        rawb,sizeof rawb))
        rawb[0]=0;
    rss_html_to_text(title,sizeof title,rawt);
    rss_html_to_text(text, sizeof text, rawb);
    if(title[0] || text[0]){ cb(title,text,ctx); (*count)++; }
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
