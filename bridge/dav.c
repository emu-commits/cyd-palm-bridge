/* dav.c -- CalDAV/CardDAV over the curl binary (host transport).
 *
 * Deliberately tiny: PUT/GET/PROPFIND are all the sync engine needs. XML is
 * scanned, not DOM-parsed -- we only pull <href> and <getetag> out of the
 * PROPFIND multistatus, which is all a member listing requires.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dav.h"

int dav_last_status = 0;   /* HTTP status of the most recent discovery request */

/* case-insensitive substring search bounded by [s,end). */
static const char* strcasestr_range(const char*s,const char*end,const char*needle){
    size_t nl=strlen(needle);
    for(const char*p=s; p+nl<=end; p++) if(strncasecmp(p,needle,nl)==0) return p;
    return NULL;
}

/* XML value extraction that ignores namespace prefixes AND attributes -- iCloud
 * writes <href xmlns="DAV:">.. so "href>" substring matching fails; match the
 * element by its local name instead. Returns content start (after '>'), NULL if
 * not found. Skips closing tags. */
static const char* xml_open_e(const char*from,const char*end,const char*name){
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
/* text content of the first element named `name` in [from,end), trimmed. 1 if found. */
static int xml_text_e(const char*from,const char*end,const char*name,char*out,int cap){
    if(out&&cap) out[0]=0;
    const char*s=xml_open_e(from,end,name); if(!s) return 0;
    const char*e=strchr(s,'<'); if(!e) e=s+strlen(s);
    while(s<e && (*s==' '||*s=='\r'||*s=='\n'||*s=='\t')) s++;
    while(e>s && (e[-1]==' '||e[-1]=='\r'||e[-1]=='\n'||e[-1]=='\t')) e--;
    int n=(int)(e-s); if(n>cap-1)n=cap-1; if(n<0)n=0; memcpy(out,s,n); out[n]=0;
    return 1;
}
static const char* xml_open(const char*from,const char*name){ return xml_open_e(from,NULL,name); }
static int xml_text(const char*from,const char*name,char*out,int cap){ return xml_text_e(from,NULL,name,out,cap); }
static void stripQuotes(char*s){
    int l=(int)strlen(s); if(l>=2 && s[0]=='"' && s[l-1]=='"'){ memmove(s,s+1,l-2); s[l-2]=0; }
    /* also strip a leading weak-etag marker W/ */
    if(!strncmp(s,"W/",2)) memmove(s,s+2,strlen(s+2)+1);
}
static void baseName(const char*full,char*out,int cap){
    const char*e=full+strlen(full); while(e>full && e[-1]=='/') e--;   /* trailing slash */
    const char*s=e; while(s>full && s[-1]!='/') s--;
    int n=(int)(e-s); if(n>cap-1)n=cap-1; if(n<0)n=0; memcpy(out,s,n); out[n]=0;
}

/* run a command, capture stdout into out (cap). returns bytes or -1. */
static int run(const char*cmd,char*out,int cap){
    FILE*p=popen(cmd,"r"); if(!p) return -1;
    int n=0; if(out&&cap>0){ n=(int)fread(out,1,cap-1,p); out[n]=0; }
    else { char sink[4096]; while(fread(sink,1,sizeof sink,p)>0){} }
    pclose(p);
    return n;
}

/* extract the LAST occurrence of an ETag from a curl -D header dump. */
static void grab_etag(const char*hdr,char*out,int cap){
    out[0]=0;
    const char*p=hdr, *found=NULL;
    while((p=strcasestr(p,"etag:"))){ found=p; p+=5; }
    if(!found){ return; }
    found+=5; while(*found==' '||*found=='"') found++;
    int i=0; while(found[i]&&found[i]!='"'&&found[i]!='\r'&&found[i]!='\n'&&i<cap-1){ out[i]=found[i]; i++; }
    out[i]=0;
}

int dav_put(const DavCtx*d,const char*coll,const char*name,const char*ctype,
            const char*bodyfile,const char*ifmatch,
            char*etag_out,int etagcap,int*status_out){
    char hdrfile[]="state/.davhdr";
    char ifhdr[256]="";
    if(ifmatch&&ifmatch[0]) snprintf(ifhdr,sizeof ifhdr,"-H 'If-Match: \"%s\"' ",ifmatch);
    char cmd[1280];
    snprintf(cmd,sizeof cmd,
        "curl -s -o /dev/null -D %s -w '%%{http_code}' -u %s:%s -X PUT "
        "-H 'Content-Type: %s' %s--data-binary @%s '%s/%s/%s'",
        hdrfile,d->user,d->pass,ctype,ifhdr,bodyfile,d->base,coll,name);
    char code[16]={0};
    if(run(cmd,code,sizeof code)<0) return -1;
    if(status_out) *status_out=atoi(code);
    if(etag_out&&etagcap>0){
        etag_out[0]=0;
        FILE*f=fopen(hdrfile,"rb"); char h[4096]={0};
        if(f){ int n=(int)fread(h,1,sizeof h-1,f); h[n]=0; fclose(f); }
        grab_etag(h,etag_out,etagcap);
    }
    return 0;
}

int dav_delete(const DavCtx*d,const char*coll,const char*name,const char*ifmatch){
    char ifhdr[256]="";
    if(ifmatch&&ifmatch[0]) snprintf(ifhdr,sizeof ifhdr,"-H 'If-Match: \"%s\"' ",ifmatch);
    char cmd[1024],code[16]={0};
    snprintf(cmd,sizeof cmd,"curl -s -o /dev/null -w '%%{http_code}' -u %s:%s %s-X DELETE '%s/%s/%s'",
        d->user,d->pass,ifhdr,d->base,coll,name);
    if(run(cmd,code,sizeof code)<0) return -1;
    return atoi(code);
}

int dav_getetag(const DavCtx*d,const char*coll,const char*name,char*etag,int cap){
    char cmd[1024];
    snprintf(cmd,sizeof cmd,
        "curl -s -u %s:%s -X PROPFIND -H 'Depth: 0' -H 'Content-Type: application/xml' "
        "--data '<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:getetag/></d:prop></d:propfind>' '%s/%s/%s'",
        d->user,d->pass,d->base,coll,name);
    char buf[8192]={0};
    if(run(cmd,buf,sizeof buf)<0){ if(etag&&cap)etag[0]=0; return -1; }
    if(!xml_text(buf,"getetag",etag,cap)){ if(etag&&cap)etag[0]=0; return -1; }
    stripQuotes(etag);
    return 0;
}

int dav_get(const DavCtx*d,const char*coll,const char*name,char*out,int cap){
    char cmd[1024];
    snprintf(cmd,sizeof cmd,"curl -s -u %s:%s '%s/%s/%s'",d->user,d->pass,d->base,coll,name);
    return run(cmd,out,cap);
}

int dav_list(const DavCtx*d,const char*coll,dav_list_cb cb,void*ctx){
    char cmd[1024];
    snprintf(cmd,sizeof cmd,
        "curl -s -u %s:%s -X PROPFIND -H 'Depth: 1' -H 'Content-Type: application/xml' "
        "--data '<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop>"
        "<d:getetag/></d:prop></d:propfind>' '%s/%s/'",
        d->user,d->pass,d->base,coll);
    char*buf=malloc(1<<20); if(!buf) return -1;
    int n=run(cmd,buf,1<<20); if(n<0){ free(buf); return -1; }

    /* walk each <response>: pull href basename + getetag                    */
    int count=0;
    const char*p=buf;
    while((p=strcasestr(p,"<response"))){
        const char*end=strcasestr(p,"</response>"); if(!end) break;
        char href[512]=""; xml_text_e(p,end,"href",href,sizeof href);
        char name[256]=""; baseName(href,name,sizeof name);
        char etag[160]=""; xml_text_e(p,end,"getetag",etag,sizeof etag); stripQuotes(etag);
        if(name[0] && cb){ cb(name,etag,ctx); count++; }
        p=end+1;
    }
    free(buf);
    return count;
}

/* ---------- shared helpers for REPORT + discovery ---------- */
static char* slurp(const char*path,int*len){
    FILE*f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); if(n<0)n=0; fseek(f,0,SEEK_SET);
    char*b=malloc((size_t)n+1); if(!b){ fclose(f); return NULL; }
    size_t got=fread(b,1,(size_t)n,f); b[got]=0; if(len)*len=(int)got; fclose(f);
    return b;
}
int dav_sync_report(const DavCtx*d,const char*coll,const char*token,
                    dav_sync_cb cb,void*ctx,char*newtoken,int tokcap){
    if(newtoken&&tokcap) newtoken[0]=0;
    /* build the request body */
    { FILE*f=fopen("state/.sreq","wb"); if(!f) return -1;
      fprintf(f,"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<d:sync-collection xmlns:d=\"DAV:\">"
        "<d:sync-token>%s</d:sync-token><d:sync-level>1</d:sync-level>"
        "<d:prop><d:getetag/></d:prop></d:sync-collection>", token?token:"");
      fclose(f); }
    char cmd[1024];
    snprintf(cmd,sizeof cmd,
        "curl -s -L -o state/.srep -w '%%{http_code}' -u %s:%s -X REPORT "
        "-H 'Depth: 1' -H 'Content-Type: application/xml' --data-binary @state/.sreq '%s/%s/'",
        d->user,d->pass,d->base,coll);
    char code[16]={0}; if(run(cmd,code,sizeof code)<0) return -1;
    int status=atoi(code);
    int len=0; char*buf=slurp("state/.srep",&len); if(!buf) return -1;

    if(strcasestr(buf,"valid-sync-token")){ free(buf); return 1; }      /* token expired */
    if(status!=207 || !strcasestr(buf,"multistatus")){ free(buf); return -1; } /* unsupported */

    const char*p=buf;
    while((p=strcasestr(p,"<response"))){
        const char*end=strcasestr(p,"</response>"); if(!end) break;
        char href[512]=""; xml_text_e(p,end,"href",href,sizeof href);
        char name[256]=""; baseName(href,name,sizeof name);
        char etag[160]=""; int hasEtag=xml_text_e(p,end,"getetag",etag,sizeof etag); stripQuotes(etag);
        /* a removed member has a 404 status and no getetag */
        const char*f404=strstr(p,"404");
        int deleted = !hasEtag && f404 && f404<end;
        if(name[0] && cb) cb(name,etag,deleted,ctx);
        p=end+1;
    }
    /* the new sync-token is a top-level element */
    if(newtoken && tokcap) xml_text(buf,"sync-token",newtoken,tokcap);
    free(buf);
    return 0;
}

/* ---------------- discovery ---------------- */
int dav_prop_href(const DavCtx*d,const char*path,const char*propOpen,
                  const char*extra_ns,char*out,int cap){
    if(out&&cap) out[0]=0;
    { FILE*f=fopen("state/.dreq","wb"); if(!f) return -1;
      fprintf(f,"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\" %s><d:prop>%s</d:prop></d:propfind>",
        extra_ns?extra_ns:"", propOpen);
      fclose(f); }
    char cmd[1024];
    snprintf(cmd,sizeof cmd,
        "curl -s -L -o state/.drep -w '%%{http_code}' -u %s:%s -X PROPFIND "
        "-H 'Depth: 0' -H 'Content-Type: application/xml' --data-binary @state/.dreq '%s%s'",
        d->user,d->pass,d->base,path);
    char code[16]={0}; if(run(cmd,code,sizeof code)<0) return -1;
    dav_last_status=atoi(code);
    int len=0; char*buf=slurp("state/.drep",&len); if(!buf) return -1;
    /* local name of the property element (strip '<' and any ns prefix) */
    char local[64]={0};
    { const char*s=propOpen; while(*s=='<') s++;
      const char*stop=s; while(*stop && *stop!=' '&&*stop!='/'&&*stop!='>') stop++;
      const char*colon=strchr(s,':'); if(colon && colon<stop) s=colon+1;
      int i=0; for(; s+i<stop && i<63; i++) local[i]=s[i]; local[i]=0; }
    /* find the property element, then its inner <href> (ns/attrs tolerant) */
    const char*el=xml_open(buf,local);
    int rc=-1;
    if(el && xml_text(el,"href",out,cap) && out[0]) rc=0;
    free(buf);
    return rc;
}

int dav_effective_host(const DavCtx*d,const char*path,char*out,int cap){
    if(out&&cap) out[0]=0;
    { FILE*f=fopen("state/.dreq","wb"); if(f){ fprintf(f,
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:current-user-principal/></d:prop></d:propfind>"); fclose(f);} }
    char cmd[1024];
    snprintf(cmd,sizeof cmd,
        "curl -s -L -o /dev/null -w '%%{url_effective}' -u %s:%s -X PROPFIND "
        "-H 'Depth: 0' -H 'Content-Type: application/xml' --data-binary @state/.dreq '%s%s'",
        d->user,d->pass,d->base,path);
    char url[512]={0}; if(run(cmd,url,sizeof url)<0) return -1;
    /* keep scheme://host (strip path) */
    const char*p=strstr(url,"://"); if(!p){ return -1; }
    const char*slash=strchr(p+3,'/'); int n = slash ? (int)(slash-url) : (int)strlen(url);
    if(n>cap-1) n=cap-1;
    memcpy(out,url,n); out[n]=0;
    return 0;
}

int dav_list_collections(const DavCtx*d,const char*path,dav_coll_cb cb,void*ctx){
    { FILE*f=fopen("state/.dreq","wb"); if(!f) return -1;
      fprintf(f,"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:displayname/></d:prop></d:propfind>");
      fclose(f); }
    char cmd[1024];
    snprintf(cmd,sizeof cmd,
        "curl -s -L -o state/.drep -w '%%{http_code}' -u %s:%s -X PROPFIND "
        "-H 'Depth: 1' -H 'Content-Type: application/xml' --data-binary @state/.dreq '%s%s'",
        d->user,d->pass,d->base,path);
    char code[16]={0}; if(run(cmd,code,sizeof code)<0) return -1;
    int len=0; char*buf=slurp("state/.drep",&len); if(!buf) return -1;
    int count=0; const char*p=buf;
    while((p=strcasestr(p,"<response"))){
        const char*end=strcasestr(p,"</response>"); if(!end) break;
        char href[512]=""; xml_text_e(p,end,"href",href,sizeof href);
        int kind=0;
        const char*rt=strcasestr(p,"resourcetype"); if(rt&&rt<end){
            const char*rte=strcasestr(rt,"/resourcetype"); if(!rte||rte>end) rte=end;
            if(strcasestr_range(rt,rte,"calendar")) kind='c';
            else if(strcasestr_range(rt,rte,"addressbook")) kind='a';
        }
        char dn[128]=""; xml_text_e(p,end,"displayname",dn,sizeof dn);
        if(href[0] && cb){ cb(href,kind,dn,ctx); count++; }
        p=end+1;
    }
    free(buf);
    return count;
}
