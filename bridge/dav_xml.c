/* dav_xml.c -- transport-independent DAV response parsing (see dav_xml.h). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dav_xml.h"
#include "dav.h"   /* dav_check: temporary heap checkpoints */

const char* dav_strcasestr_range(const char*s,const char*end,const char*needle){
    size_t nl=strlen(needle);
    for(const char*p=s; p+nl<=end; p++) if(strncasecmp(p,needle,nl)==0) return p;
    return NULL;
}

const char* dav_xml_open(const char*from,const char*end,const char*name){
    size_t nl=strlen(name);
    for(const char*p=from; p && (!end||p<end) && (p=strchr(p,'<')); p++){
        if(end && p>=end) break;
        const char*q=p+1; if(*q=='/'||*q=='!'||*q=='?') continue;   /* closing / comment / decl */
        const char*e=q, *colon=NULL;
        while(*e && *e!='>' && *e!=' ' && *e!='\t' && *e!='/'){ if(*e==':') colon=e; e++; }
        const char*loc = colon?colon+1:q; size_t ll=(size_t)(e-loc);
        if(ll==nl && strncasecmp(loc,name,nl)==0){
            const char*gt=strchr(e,'>'); if(!gt||(end&&gt>=end)) return NULL;
            return gt+1;                        /* self-closing yields "" text */
        }
    }
    return NULL;
}

int dav_xml_text(const char*from,const char*end,const char*name,char*out,int cap){
    if(out&&cap) out[0]=0;
    const char*s=dav_xml_open(from,end,name); if(!s) return 0;
    const char*e=strchr(s,'<'); if(!e) e=s+strlen(s);
    while(s<e && (*s==' '||*s=='\r'||*s=='\n'||*s=='\t')) s++;
    while(e>s && (e[-1]==' '||e[-1]=='\r'||e[-1]=='\n'||e[-1]=='\t')) e--;
    int n=(int)(e-s); if(n>cap-1)n=cap-1; if(n<0)n=0; memcpy(out,s,n); out[n]=0;
    return 1;
}

void dav_strip_quotes(char*s){
    if(!strncmp(s,"W/",2)) memmove(s,s+2,strlen(s+2)+1);   /* the weak-etag marker first: W/"x" */
    int l=(int)strlen(s); if(l>=2 && s[0]=='"' && s[l-1]=='"'){ memmove(s,s+1,l-2); s[l-2]=0; }
}

void dav_basename(const char*full,char*out,int cap){
    const char*e=full+strlen(full); while(e>full && e[-1]=='/') e--;   /* trailing slash */
    const char*s=e; while(s>full && s[-1]!='/') s--;
    int n=(int)(e-s); if(n>cap-1)n=cap-1; if(n<0)n=0; memcpy(out,s,n); out[n]=0;
}

int dav_href_is_coll(const char*h){ int n=(int)strlen(h); return n>0 && h[n-1]=='/'; }

/* ---------------------- high-level parsers ---------------------- */
int dav_parse_members(const char*buf,dav_list_cb cb,void*ctx){
    int count=0; const char*p=buf;
    while((p=strcasestr(p,"<response"))){
        const char*end=strcasestr(p,"</response>"); if(!end) break;
        char href[512]=""; dav_xml_text(p,end,"href",href,sizeof href);
        if(dav_href_is_coll(href)){ p=end+1; continue; }        /* skip the collection self */
        char name[256]=""; dav_basename(href,name,sizeof name);
        char etag[160]=""; dav_xml_text(p,end,"getetag",etag,sizeof etag); dav_strip_quotes(etag);
        if(name[0] && cb){ cb(name,etag,ctx); count++; }
        p=end+1;
    }
    return count;
}

int dav_parse_report(const char*buf,int status,dav_sync_cb cb,void*ctx,
                     char*newtoken,int tokcap){
    if(newtoken&&tokcap) newtoken[0]=0;
    if(strcasestr(buf,"valid-sync-token")) return 1;                       /* token expired */
    if(status!=207 || !strcasestr(buf,"multistatus")) return -1;          /* unsupported   */

    const char*p=buf;
    while((p=strcasestr(p,"<response"))){
        const char*end=strcasestr(p,"</response>"); if(!end) break;
        char href[512]=""; dav_xml_text(p,end,"href",href,sizeof href);
        if(dav_href_is_coll(href)){ p=end+1; continue; }        /* skip the collection self */
        char name[256]=""; dav_basename(href,name,sizeof name);
        char etag[160]=""; int hasEtag=dav_xml_text(p,end,"getetag",etag,sizeof etag); dav_strip_quotes(etag);
        /* a removed member has a 404 status and no getetag */
        const char*f404=strstr(p,"404");
        int deleted = !hasEtag && f404 && f404<end;
        if(name[0] && cb) cb(name,etag,deleted,ctx);
        p=end+1;
    }
    if(newtoken && tokcap) dav_xml_text(buf,NULL,"sync-token",newtoken,tokcap);
    return 0;
}

/* ---- streaming variants: slide a window over a spooled response FILE so a large
 * collection's member list never needs a full-body RAM buffer. A single
 * <response>..</response> block (href + getetag) is a few hundred bytes, far under
 * the window, so blocks are always processed whole; the sync-token, which trails
 * the last </response>, is left in the final tail and read there. ---- */
#ifndef DAV_STREAM_WIN
#define DAV_STREAM_WIN 4096
#endif
#define RESP_TAG_LEN 11   /* strlen("</response>") */

int dav_parse_report_stream(FILE*f,int status,dav_sync_cb cb,void*ctx,
                            char*newtoken,int tokcap){
    if(newtoken&&tokcap) newtoken[0]=0;
    if(status!=207) return -1;
    char*buf=malloc(DAV_STREAM_WIN); if(!buf) return -1;
    int len=0, first=1, sawMulti=0, chkw=0;
    for(;;){
        dav_check("rep-win");
        int got=(int)fread(buf+len,1,(size_t)(DAV_STREAM_WIN-1-len),f);
        len+=got; buf[len]=0;
        dav_check("rep-win-read");
        chkw++;
        if(first){
            if(strcasestr(buf,"valid-sync-token")){ free(buf); return 1; }  /* token expired */
            if(strcasestr(buf,"multistatus")) sawMulti=1;
            first=0;
        }
        const char*p=buf;
        for(;;){
            const char*r=strcasestr(p,"<response");
            if(!r) break;                              /* keep the tail after the last
                                                          </response> -- it holds the
                                                          trailing sync-token */
            const char*end=strcasestr(r,"</response>");
            if(!end){ p=r; break; }                    /* incomplete block: keep from here */
            char href[512]=""; dav_xml_text(r,end,"href",href,sizeof href);
            if(!dav_href_is_coll(href)){
                char name[256]=""; dav_basename(href,name,sizeof name);
                char etag[160]=""; int hasEtag=dav_xml_text(r,end,"getetag",etag,sizeof etag); dav_strip_quotes(etag);
                const char*f404=strstr(r,"404"); int deleted=!hasEtag && f404 && f404<end;
                if(name[0] && cb){ cb(name,etag,deleted,ctx); dav_check("rep-cb"); }
            }
            p=end+RESP_TAG_LEN;
        }
        int consumed=(int)(p-buf), tail=len-consumed;
        if(tail>0 && consumed>0) memmove(buf,buf+consumed,(size_t)tail);
        len = tail>0 ? tail : 0; buf[len]=0;
        if(got==0) break;                              /* EOF: final tail holds the token */
        if(len>=DAV_STREAM_WIN-1){ len=0; buf[0]=0; }  /* safety: oversized block -> skip */
    }
    dav_check("rep-loop-done");
    if(newtoken&&tokcap) dav_xml_text(buf,NULL,"sync-token",newtoken,tokcap);
    dav_check("rep-token-read");
    (void)chkw;
    int rc = sawMulti ? 0 : -1;
    free(buf);
    return rc;
}

int dav_parse_members_stream(FILE*f,dav_list_cb cb,void*ctx){
    char*buf=malloc(DAV_STREAM_WIN); if(!buf) return -1;
    int len=0, first=1, sawMulti=0, count=0;
    for(;;){
        int got=(int)fread(buf+len,1,(size_t)(DAV_STREAM_WIN-1-len),f);
        len+=got; buf[len]=0;
        if(first){ if(strcasestr(buf,"multistatus")) sawMulti=1; first=0; }
        const char*p=buf;
        for(;;){
            const char*r=strcasestr(p,"<response");
            if(!r) break;                              /* keep the tail: it may hold a
                                                          partial "<respo…" that strcasestr
                                                          can't match until more is read */
            const char*end=strcasestr(r,"</response>");
            if(!end){ p=r; break; }
            char href[512]=""; dav_xml_text(r,end,"href",href,sizeof href);
            if(!dav_href_is_coll(href)){
                char name[256]=""; dav_basename(href,name,sizeof name);
                char etag[160]=""; dav_xml_text(r,end,"getetag",etag,sizeof etag); dav_strip_quotes(etag);
                if(name[0] && cb){ cb(name,etag,ctx); count++; }
            }
            p=end+RESP_TAG_LEN;
        }
        int consumed=(int)(p-buf), tail=len-consumed;
        if(tail>0 && consumed>0) memmove(buf,buf+consumed,(size_t)tail);
        len = tail>0 ? tail : 0; buf[len]=0;
        if(got==0) break;
        if(len>=DAV_STREAM_WIN-1){ len=0; buf[0]=0; }
    }
    int rc = sawMulti ? count : -1;
    free(buf);
    return rc;
}

/* streaming variant of dav_parse_collections: slide a window over a spooled
 * home-set PROPFIND so a calendar home with many collections (iCloud returns
 * calendars + reminder lists + inbox/outbox/notification) is never truncated by
 * a fixed RAM buffer. A single collection <response> block (resourcetype +
 * displayname) is well under the window, so blocks are processed whole. */
int dav_parse_collections_stream(FILE*f,dav_coll_cb cb,void*ctx){
    char*buf=malloc(DAV_STREAM_WIN); if(!buf) return -1;
    int len=0, count=0;
    for(;;){
        int got=(int)fread(buf+len,1,(size_t)(DAV_STREAM_WIN-1-len),f);
        len+=got; buf[len]=0;
        const char*p=buf;
        for(;;){
            const char*r=strcasestr(p,"<response");
            if(!r) break;                          /* keep the tail (may be a partial tag) */
            const char*end=strcasestr(r,"</response>");
            if(!end){ p=r; break; }                /* incomplete block: keep from here */
            char href[512]=""; dav_xml_text(r,end,"href",href,sizeof href);
            int kind=0;
            const char*rt=dav_strcasestr_range(r,end,"resourcetype");
            if(rt){
                const char*rte=dav_strcasestr_range(rt,end,"/resourcetype"); if(!rte) rte=end;
                if(dav_strcasestr_range(rt,rte,"calendar")) kind='c';
                else if(dav_strcasestr_range(rt,rte,"addressbook")) kind='a';
            }
            char dn[128]=""; dav_xml_text(r,end,"displayname",dn,sizeof dn);
            if(href[0] && cb){ cb(href,kind,dn,ctx); count++; }
            p=end+RESP_TAG_LEN;
        }
        int consumed=(int)(p-buf), tail=len-consumed;
        if(tail>0 && consumed>0) memmove(buf,buf+consumed,(size_t)tail);
        len = tail>0 ? tail : 0; buf[len]=0;
        if(got==0) break;
        if(len>=DAV_STREAM_WIN-1){ len=0; buf[0]=0; }  /* safety: oversized block -> skip */
    }
    free(buf);
    return count;
}

int dav_parse_collections(const char*buf,dav_coll_cb cb,void*ctx){
    int count=0; const char*p=buf;
    while((p=strcasestr(p,"<response"))){
        const char*end=strcasestr(p,"</response>"); if(!end) break;
        char href[512]=""; dav_xml_text(p,end,"href",href,sizeof href);
        int kind=0;
        const char*rt=strcasestr(p,"resourcetype"); if(rt&&rt<end){
            const char*rte=strcasestr(rt,"/resourcetype"); if(!rte||rte>end) rte=end;
            if(dav_strcasestr_range(rt,rte,"calendar")) kind='c';
            else if(dav_strcasestr_range(rt,rte,"addressbook")) kind='a';
        }
        char dn[128]=""; dav_xml_text(p,end,"displayname",dn,sizeof dn);
        if(href[0] && cb){ cb(href,kind,dn,ctx); count++; }
        p=end+1;
    }
    return count;
}

int dav_parse_prop_href(const char*buf,const char*propOpen,char*out,int cap){
    if(out&&cap) out[0]=0;
    /* local name of the property element (strip '<' and any ns prefix) */
    char local[64]={0};
    { const char*s=propOpen; while(*s=='<') s++;
      const char*stop=s; while(*stop && *stop!=' '&&*stop!='/'&&*stop!='>') stop++;
      const char*colon=strchr(s,':'); if(colon && colon<stop) s=colon+1;
      int i=0; for(; s+i<stop && i<63; i++) local[i]=s[i]; local[i]=0; }
    const char*el=dav_xml_open(buf,NULL,local);
    if(el && dav_xml_text(el,NULL,"href",out,cap) && out[0]) return 0;
    return -1;
}

/* ---------------------- a time window, and multiget ---------------------- */
#include <time.h>

static void utc_stamp(long long t,char*out,int cap){
    time_t tt=(time_t)t; struct tm tm; gmtime_r(&tt,&tm);
    unsigned y=(unsigned)(tm.tm_year+1900)%10000u, mo=(unsigned)(tm.tm_mon+1)%100u,
             dd=(unsigned)tm.tm_mday%100u, h=(unsigned)tm.tm_hour%100u,
             mi=(unsigned)tm.tm_min%100u, se=(unsigned)tm.tm_sec%100u;
    snprintf(out,cap,"%04u%02u%02uT%02u%02u%02uZ",y,mo,dd,h,mi,se);
}

int dav_body_window(char*out,int cap,long long start,long long end){
    char a[32],b[32]; utc_stamp(start,a,sizeof a); utc_stamp(end,b,sizeof b);
    int n=snprintf(out,cap,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
        "<D:prop><D:getetag/></D:prop>"
        "<C:filter><C:comp-filter name=\"VCALENDAR\"><C:comp-filter name=\"VEVENT\">"
        "<C:time-range start=\"%s\" end=\"%s\"/>"
        "</C:comp-filter></C:comp-filter></C:filter></C:calendar-query>",a,b);
    return (n<0||n>=cap) ? -1 : n;
}

/* append s to out[*n], XML-escaping &, < and >; 0 ok, -1 if it doesn't fit */
static int put_esc(char*out,int cap,int*n,const char*s){
    for(;*s;s++){
        const char*r = *s=='&'?"&amp;" : *s=='<'?"&lt;" : *s=='>'?"&gt;" : NULL;
        int l = r ? (int)strlen(r) : 1;
        if(*n+l>=cap) return -1;
        if(r){ memcpy(out+*n,r,(size_t)l); } else out[*n]=*s;
        *n+=l;
    }
    out[*n]=0; return 0;
}

int dav_body_multiget(char*out,int cap,const char*base,const char*coll,int card,
                      const char*const*names,int n){
    /* the path part of the base URL: "https://host/x" -> "/x", "https://host" -> "" */
    const char*bp=strstr(base,"://"); bp = bp ? strchr(bp+3,'/') : NULL; if(!bp) bp="";
    size_t bl=strlen(bp); while(bl && bp[bl-1]=='/') bl--;
    const char*ns = card ? "urn:ietf:params:xml:ns:carddav" : "urn:ietf:params:xml:ns:caldav";
    int len=snprintf(out,cap,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<C:%s xmlns:D=\"DAV:\" xmlns:C=\"%s\"><D:prop><D:getetag/><C:%s/></D:prop>",
        card?"addressbook-multiget":"calendar-multiget", ns, card?"address-data":"calendar-data");
    if(len<0||len>=cap) return -1;
    for(int i=0;i<n;i++){
        char path[512];
        snprintf(path,sizeof path,"%.*s/%s/%s",(int)bl,bp,coll[0]=='/'?coll+1:coll,names[i]);
        if(len+12>=cap) return -1;
        memcpy(out+len,"<D:href>",8); len+=8; out[len]=0;
        if(put_esc(out,cap,&len,path)!=0) return -1;
        if(len+10>=cap) return -1;
        memcpy(out+len,"</D:href>",9); len+=9; out[len]=0;
    }
    int t=snprintf(out+len,cap-len,"</C:%s>",card?"addressbook-multiget":"calendar-multiget");
    if(t<0||len+t>=cap) return -1;
    return len+t;
}

/* decode the entity after '&' (read from in) into one byte (or UTF-8 bytes
 * for a numeric one) written through put(); an unknown one is passed through */
static int ent_decode(FILE*in,char*ob,int obcap){
    char e[12]; int n=0, c;
    while(n<(int)sizeof e-1 && (c=fgetc(in))!=EOF && c!=';'){ e[n++]=(char)c; if(c=='<'||c=='&'){ break; } }
    e[n]=0;
    if(!strcmp(e,"lt")){ ob[0]='<'; return 1; }
    if(!strcmp(e,"gt")){ ob[0]='>'; return 1; }
    if(!strcmp(e,"amp")){ ob[0]='&'; return 1; }
    if(!strcmp(e,"quot")){ ob[0]='"'; return 1; }
    if(!strcmp(e,"apos")){ ob[0]='\''; return 1; }
    if(e[0]=='#'){
        unsigned long v = (e[1]=='x'||e[1]=='X') ? strtoul(e+2,NULL,16) : strtoul(e+1,NULL,10);
        if(v<0x80){ ob[0]=(char)v; return 1; }
        if(v<0x800){ ob[0]=(char)(0xC0|(v>>6)); ob[1]=(char)(0x80|(v&0x3F)); return 2; }
        if(v<0x10000){ ob[0]=(char)(0xE0|(v>>12)); ob[1]=(char)(0x80|((v>>6)&0x3F)); ob[2]=(char)(0x80|(v&0x3F)); return 3; }
        ob[0]=(char)(0xF0|(v>>18)); ob[1]=(char)(0x80|((v>>12)&0x3F)); ob[2]=(char)(0x80|((v>>6)&0x3F)); ob[3]=(char)(0x80|(v&0x3F)); return 4;
    }
    /* not an entity we know: keep it as written */
    int k=snprintf(ob,obcap,"&%s;",e); return k<obcap?k:obcap-1;
}

int dav_parse_multiget_stream(FILE*in,FILE*body,dav_obj_cb cb,void*ctx){
    char tag[128], txt[512], href[512]="", etag[160]="";
    int tl=0, cap=0;               /* capturing text: 1 href, 2 getetag */
    int inResp=0, inData=0, hasData=0, sawMulti=0, count=0;
    long off=0, len=0;
    int c;
    while((c=fgetc(in))!=EOF){
        if(c=='<'){
            /* a tag, a CDATA section, a comment or a declaration */
            int n=0, d;
            while(n<8 && (d=fgetc(in))!=EOF){ tag[n++]=(char)d; if(d=='>') break; }
            tag[n]=0;
            if(n>=8 && !strncmp(tag,"![CDATA[",8)){
                /* raw text up to "]]>": hold back up to two ']' until we know */
                int pend=0;
                while((d=fgetc(in))!=EOF){
                    if(d=='>' && pend==2) break;
                    if(d==']' && pend<2){ pend++; continue; }
                    char out3[3]; int k=0;
                    if(d==']'){ out3[k++]=']'; }            /* a third ']': the oldest is text */
                    else { while(pend){ out3[k++]=']'; pend--; } out3[k++]=(char)d; }
                    for(int i2=0;i2<k;i2++){
                        if(inData){ fputc(out3[i2],body); len++; }
                        else if(cap && tl<(int)sizeof txt-1) txt[tl++]=out3[i2];
                    }
                }
                continue;
            }
            if(n>=3 && !strncmp(tag,"!--",3)){          /* a comment: skip to "-->" */
                if(n>=6 && !strncmp(tag+n-3,"-->",3)) continue;    /* "<!---->", already read */
                int p1=0,p2=0;
                while((d=fgetc(in))!=EOF){ if(d=='>' && p1=='-' && p2=='-') break; p1=p2; p2=d; }
                continue;
            }
            /* the rest of the tag, up to '>' (attributes may be long: drop them) */
            if(!(n>0 && tag[n-1]=='>')){
                while((d=fgetc(in))!=EOF && d!='>'){ if(n<(int)sizeof tag-1) tag[n++]=(char)d; }
                tag[n]=0;
            } else tag[--n]=0;
            if(tag[0]=='?'||tag[0]=='!') continue;      /* declaration */
            int closing = tag[0]=='/';
            int selfc = n>0 && tag[n-1]=='/';
            const char*nm = tag+closing; const char*e=nm; const char*colon=NULL;
            while(*e && *e!=' ' && *e!='\t' && *e!='\r' && *e!='\n' && *e!='/' ){ if(*e==':') colon=e; e++; }
            const char*loc = colon?colon+1:nm; int ll=(int)(e-loc);
            #define IS(s) (ll==(int)strlen(s) && !strncasecmp(loc,s,(size_t)ll))
            if(IS("multistatus")){ if(!closing) sawMulti=1; }
            else if(IS("response")){
                if(!closing && !selfc){ inResp=1; href[0]=etag[0]=0; hasData=0; }
                else if(closing && inResp){
                    inResp=0;
                    if(hasData && href[0] && !dav_href_is_coll(href)){
                        char name[256]; dav_basename(href,name,sizeof name);
                        if(name[0] && cb){ cb(name,etag,off,len,ctx); count++; }
                    }
                }
            }
            else if(inResp && (IS("calendar-data")||IS("address-data"))){
                if(!closing && !selfc){ inData=1; fflush(body); off=ftell(body); len=0; }
                else if(closing && inData){ inData=0; hasData=1; }
            }
            else if(inResp && !inData && IS("href")){
                if(!closing && !selfc){ cap=1; tl=0; }
                else if(closing && cap==1){ txt[tl]=0; if(!href[0]){ char*s=txt; while(*s==' '||*s=='\n'||*s=='\r'||*s=='\t') s++;
                    snprintf(href,sizeof href,"%s",s); int hl=(int)strlen(href);
                    while(hl && (href[hl-1]==' '||href[hl-1]=='\n'||href[hl-1]=='\r'||href[hl-1]=='\t')) href[--hl]=0; } cap=0; }
            }
            else if(inResp && !inData && IS("getetag")){
                if(!closing && !selfc){ cap=2; tl=0; }
                else if(closing && cap==2){ txt[tl]=0; snprintf(etag,sizeof etag,"%.159s",txt); dav_strip_quotes(etag); cap=0; }
            }
            #undef IS
            continue;
        }
        if(c=='&'){
            char ob[16]; int k=ent_decode(in,ob,sizeof ob);
            if(inData){ fwrite(ob,1,(size_t)k,body); len+=k; }
            else if(cap){ for(int i=0;i<k && tl<(int)sizeof txt-1;i++) txt[tl++]=ob[i]; }
            continue;
        }
        if(inData){ fputc(c,body); len++; }
        else if(cap && tl<(int)sizeof txt-1) txt[tl++]=(char)c;
    }
    fflush(body);
    return sawMulti ? count : -1;
}
