/* dav.c -- CalDAV/CardDAV over the curl binary (host transport).
 *
 * Deliberately tiny: PUT/GET/PROPFIND are all the sync engine needs. XML is
 * scanned, not DOM-parsed -- we only pull <href> and <getetag> out of the
 * PROPFIND multistatus, which is all a member listing requires.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dav.h"

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
    const char*e=strcasestr(buf,"getetag>"); if(!e){ if(etag&&cap)etag[0]=0; return -1; }
    e+=8; const char*ee=strchr(e,'<'); int i=0;
    for(const char*q=e;q<ee&&i<cap-1;q++){ if(*q!='"') etag[i++]=*q; } etag[i]=0;
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
        /* href */
        const char*h=strcasestr(p,"href>"); char name[256]="";
        if(h&&h<end){ h+=5; const char*he=strchr(h,'<');
            const char*slash=he; while(slash>h && slash[-1]!='/') slash--;
            int i=0; for(const char*q=slash;q<he&&i<255;q++) name[i++]=*q; name[i]=0; }
        /* etag */
        const char*e=strcasestr(p,"getetag>"); char etag[128]="";
        if(e&&e<end){ e+=8; const char*ee=strchr(e,'<'); int i=0;
            for(const char*q=e;q<ee&&i<127;q++){ if(*q!='"') etag[i++]=*q; } etag[i]=0; }
        /* skip the collection itself (empty basename) */
        if(name[0] && strcmp(name,"")!=0 && cb){ cb(name,etag,ctx); count++; }
        p=end+1;
    }
    free(buf);
    return count;
}
