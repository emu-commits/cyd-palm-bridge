/* dav_xml.c -- transport-independent DAV response parsing (see dav_xml.h). */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dav_xml.h"

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
    int l=(int)strlen(s); if(l>=2 && s[0]=='"' && s[l-1]=='"'){ memmove(s,s+1,l-2); s[l-2]=0; }
    if(!strncmp(s,"W/",2)) memmove(s,s+2,strlen(s+2)+1);   /* leading weak-etag marker */
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
