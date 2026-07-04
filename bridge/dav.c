/* dav.c -- CalDAV/CardDAV over the curl binary (host transport).
 *
 * Deliberately tiny: PUT/GET/PROPFIND/REPORT are all the sync engine needs.
 * The curl invocation lives here; response parsing is shared with the on-device
 * mbedTLS transport (dav_esp.c) via dav_xml.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dav.h"
#include "dav_xml.h"

int dav_last_status = 0;   /* HTTP status of the most recent discovery request */

/* run a command, capture stdout into out (cap). returns bytes or -1. */
static int run(const char*cmd,char*out,int cap){
    FILE*p=popen(cmd,"r"); if(!p) return -1;
    int n=0; if(out&&cap>0){ n=(int)fread(out,1,cap-1,p); out[n]=0; }
    else { char sink[4096]; while(fread(sink,1,sizeof sink,p)>0){} }
    pclose(p);
    return n;
}

static char* slurp(const char*path,int*len){
    FILE*f=fopen(path,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); long n=ftell(f); if(n<0)n=0; fseek(f,0,SEEK_SET);
    char*b=malloc((size_t)n+1); if(!b){ fclose(f); return NULL; }
    size_t got=fread(b,1,(size_t)n,f); b[got]=0; if(len)*len=(int)got; fclose(f);
    return b;
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
    if(!dav_xml_text(buf,NULL,"getetag",etag,cap)){ if(etag&&cap)etag[0]=0; return -1; }
    dav_strip_quotes(etag);
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
    int count=dav_parse_members(buf,cb,ctx);
    free(buf);
    return count;
}

int dav_sync_report(const DavCtx*d,const char*coll,const char*token,
                    dav_sync_cb cb,void*ctx,char*newtoken,int tokcap){
    if(newtoken&&tokcap) newtoken[0]=0;
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
    char*buf=slurp("state/.srep",NULL); if(!buf) return -1;
    int rc=dav_parse_report(buf,status,cb,ctx,newtoken,tokcap);
    free(buf);
    return rc;
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
    char*buf=slurp("state/.drep",NULL); if(!buf) return -1;
    int rc=dav_parse_prop_href(buf,propOpen,out,cap);
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
    char*buf=slurp("state/.drep",NULL); if(!buf) return -1;
    int count=dav_parse_collections(buf,cb,ctx);
    free(buf);
    return count;
}
