/* dav_esp.c -- on-device CalDAV/CardDAV transport (ESP32, mbedTLS).
 *
 * Implements the same dav.h contract as the host's curl-based dav.c, but over
 * esp_http_client (TLS via the Mozilla cert bundle, Basic auth, chunked decode
 * and redirects handled by the client). Response parsing is shared with the
 * host through dav_xml.c -- this file is only the transport.
 *
 * Verified: esp_http_client's method enum includes PROPFIND and REPORT, so the
 * WebDAV verbs the sync engine needs map directly onto it.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "dav.h"
#include "dav_xml.h"

static const char *TAG = "dav";
int dav_last_status = 0;

/* accumulates the response body into a caller-owned buffer + grabs the ETag. */
typedef struct {
    char *buf; int len, cap; int truncated;
    char etag[160];
} RespAcc;

static esp_err_t on_event(esp_http_client_event_t *e){
    RespAcc *a = (RespAcc*)e->user_data;
    if(!a) return ESP_OK;
    switch(e->event_id){
    case HTTP_EVENT_ON_HEADER:
        if(e->header_key && strcasecmp(e->header_key,"ETag")==0 && e->header_value)
            snprintf(a->etag,sizeof a->etag,"%s",e->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        if(a->buf && a->cap>0){
            int room = a->cap-1 - a->len;
            int n = e->data_len < room ? e->data_len : room;
            if(n>0){ memcpy(a->buf+a->len, e->data, n); a->len+=n; a->buf[a->len]=0; }
            if(e->data_len > n) a->truncated=1;
        }
        break;
    default: break;
    }
    return ESP_OK;
}

/* Build "Basic base64(user:pass)" for a proactive Authorization header (iCloud
 * wants auth on the first request rather than after a 401 challenge). */
static void basic_auth(const DavCtx*d, char*out, int cap){
    char up[192]; int n=snprintf(up,sizeof up,"%s:%s",d->user,d->pass);
    unsigned char b64[288]; size_t bl=0;
    if(mbedtls_base64_encode(b64,sizeof b64,&bl,(const unsigned char*)up,(size_t)n)!=0){ if(cap)out[0]=0; return; }
    snprintf(out,cap,"Basic %.*s",(int)bl,(const char*)b64);
}

/* One request. `url` is absolute (scheme://host[:port]/path). body may be NULL.
 * depth: 0/1 => send that Depth header; <0 => none. Returns HTTP status or -1.
 * On success, resp holds the (truncated-to-cap) body, *respn its length, etag the
 * response ETag, effurl the final URL after any redirects. */
static int davreq(const DavCtx*d, esp_http_client_method_t method, const char*url,
                  int depth, const char*ctype, const char*ifmatch,
                  const char*body, int bodylen,
                  char*resp, int respcap, int*respn,
                  char*etag, int etagcap, char*effurl, int effcap){
    RespAcc acc = { .buf=resp, .cap=respcap };
    if(resp && respcap) resp[0]=0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_event,
        .user_data = &acc,
        .timeout_ms = 20000,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if(!c) return -1;

    char auth[320]; basic_auth(d,auth,sizeof auth);
    if(auth[0]) esp_http_client_set_header(c,"Authorization",auth);
    if(ctype)   esp_http_client_set_header(c,"Content-Type",ctype);
    if(depth>=0){ char dh[4]={ depth?'1':'0', 0 }; esp_http_client_set_header(c,"Depth",dh); }
    if(ifmatch && ifmatch[0]){ char v[176]; snprintf(v,sizeof v,"\"%s\"",ifmatch);
        esp_http_client_set_header(c,"If-Match",v); }
    if(body) esp_http_client_set_post_field(c, body, bodylen);

    esp_err_t err = esp_http_client_perform(c);
    int status = -1;
    if(err==ESP_OK){
        status = esp_http_client_get_status_code(c);
        if(effurl && effcap) esp_http_client_get_url(c, effurl, effcap);
    } else {
        ESP_LOGW(TAG,"%s failed: %s", url, esp_err_to_name(err));
    }
    if(etag && etagcap) snprintf(etag,etagcap,"%s",acc.etag);
    if(respn) *respn = acc.len;
    if(acc.truncated) ESP_LOGW(TAG,"response truncated at %d bytes: %s", respcap, url);
    esp_http_client_cleanup(c);
    return status;
}

/* ---- request-body builders (identical XML to the host) ---- */
static int body_getetag(char*b,int cap){
    return snprintf(b,cap,"<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\">"
        "<d:prop><d:getetag/></d:prop></d:propfind>");
}

/* ============================ dav.h API ============================ */

int dav_put(const DavCtx*d,const char*coll,const char*name,const char*ctype,
            const char*bodyfile,const char*ifmatch,
            char*etag_out,int etagcap,int*status_out){
    /* read the body file into memory (bodies are small: one ICS/vCard) */
    FILE*f=fopen(bodyfile,"rb"); if(!f) return -1;
    fseek(f,0,SEEK_END); long n=ftell(f); if(n<0)n=0; fseek(f,0,SEEK_SET);
    char*body=malloc((size_t)n+1); if(!body){ fclose(f); return -1; }
    size_t got=fread(body,1,(size_t)n,f); body[got]=0; fclose(f);

    char url[512]; snprintf(url,sizeof url,"%s/%s/%s",d->base,coll,name);
    if(etag_out&&etagcap) etag_out[0]=0;
    int st=davreq(d,HTTP_METHOD_PUT,url,-1,ctype,ifmatch,body,(int)got,
                  NULL,0,NULL, etag_out,etagcap, NULL,0);
    free(body);
    if(st<0) return -1;
    if(status_out) *status_out=st;
    if(etag_out&&etagcap&&!etag_out[0]) dav_getetag(d,coll,name,etag_out,etagcap); /* fallback */
    else if(etag_out) dav_strip_quotes(etag_out);
    return 0;
}

int dav_delete(const DavCtx*d,const char*coll,const char*name,const char*ifmatch){
    char url[512]; snprintf(url,sizeof url,"%s/%s/%s",d->base,coll,name);
    return davreq(d,HTTP_METHOD_DELETE,url,-1,NULL,ifmatch,NULL,0, NULL,0,NULL, NULL,0, NULL,0);
}

int dav_getetag(const DavCtx*d,const char*coll,const char*name,char*etag,int cap){
    char url[512]; snprintf(url,sizeof url,"%s/%s/%s",d->base,coll,name);
    char body[128]; int bl=body_getetag(body,sizeof body);
    char buf[1024]; int rn=0;
    int st=davreq(d,HTTP_METHOD_PROPFIND,url,0,"application/xml",NULL,body,bl,
                  buf,sizeof buf,&rn, NULL,0, NULL,0);
    if(st<0){ if(etag&&cap)etag[0]=0; return -1; }
    if(!dav_xml_text(buf,NULL,"getetag",etag,cap)){ if(etag&&cap)etag[0]=0; return -1; }
    dav_strip_quotes(etag);
    return 0;
}

int dav_get(const DavCtx*d,const char*coll,const char*name,char*out,int cap){
    char url[512]; snprintf(url,sizeof url,"%s/%s/%s",d->base,coll,name);
    int rn=0;
    int st=davreq(d,HTTP_METHOD_GET,url,-1,NULL,NULL,NULL,0, out,cap,&rn, NULL,0, NULL,0);
    if(st<0 || st>=400) return -1;
    return rn;
}

/* response buffer for listings; heap (freed per call) so it doesn't compete with
 * the ~40KB TLS handshake for long. A personal calendar's etag-only REPORT is a
 * few KB; oversize responses log a truncation warning. */
#ifndef DAV_LIST_CAP
#define DAV_LIST_CAP (8*1024)    /* malloc'd + freed per call. DELIBERATELY SMALL:
                                    on the no-PSRAM heap this buffer competes with
                                    mbedTLS's ~16.7 KB per-record INPUT buffer
                                    (DYNAMIC_BUFFER reallocs it each read) -- a
                                    12/16 KB list buffer fragmented the heap so the
                                    TLS alloc(16749) failed (ESP_ERR_HTTP_FETCH_
                                    HEADER) on the first/full REPORT. 8 KB leaves
                                    room. A response that fills it is TRUNCATED
                                    (incomplete) -> the caller treats that as a
                                    failure and falls back to the lighter PROPFIND
                                    (etags only), which fits far more records/KB.
                                    (A collection whose etag list exceeds 8 KB
                                    still needs true streaming -- see NEXT_STEPS.) */
#endif
/* a response that reached cap-1 bytes was cut off: the parse saw only part of the
 * collection, so it must NOT be trusted (acting on a partial view mass-deletes). */
#define DAV_TRUNCATED(rn) ((rn) >= DAV_LIST_CAP-1)

int dav_list(const DavCtx*d,const char*coll,dav_list_cb cb,void*ctx){
    char url[512]; snprintf(url,sizeof url,"%s/%s/",d->base,coll);
    char body[128]; int bl=body_getetag(body,sizeof body);
    char*buf=malloc(DAV_LIST_CAP); if(!buf) return -1;
    int rn=0;
    int st=davreq(d,HTTP_METHOD_PROPFIND,url,1,"application/xml",NULL,body,bl,
                  buf,DAV_LIST_CAP,&rn, NULL,0, NULL,0);
    int count = st<0 ? -1 : dav_parse_members(buf,cb,ctx);
    if(count>=0 && DAV_TRUNCATED(rn)){ count=-1; ESP_LOGW(TAG,"PROPFIND %s truncated at %d bytes -> failed",coll,rn); }
    fprintf(stderr,"[dav] PROPFIND %s -> st=%d rn=%d members=%d\n",coll,st,rn,count);
    free(buf);
    return count;
}

int dav_sync_report(const DavCtx*d,const char*coll,const char*token,
                    dav_sync_cb cb,void*ctx,char*newtoken,int tokcap){
    if(newtoken&&tokcap) newtoken[0]=0;
    char body[1400];
    int bl=snprintf(body,sizeof body,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:sync-collection xmlns:d=\"DAV:\">"
        "<d:sync-token>%s</d:sync-token><d:sync-level>1</d:sync-level>"
        "<d:prop><d:getetag/></d:prop></d:sync-collection>", token?token:"");
    char url[512]; snprintf(url,sizeof url,"%s/%s/",d->base,coll);
    char*buf=malloc(DAV_LIST_CAP); if(!buf) return -1;
    int rn=0;
    int st=davreq(d,HTTP_METHOD_REPORT,url,1,"application/xml",NULL,body,bl,
                  buf,DAV_LIST_CAP,&rn, NULL,0, NULL,0);
    int rc = st<0 ? -1 : dav_parse_report(buf,st,cb,ctx,newtoken,tokcap);
    /* truncated => incomplete view + the trailing sync-token wasn't received, so
     * fail: the caller falls back to the lighter PROPFIND (etags only, no bodies)
     * or, failing that, skips the collection rather than acting on partial data. */
    if(rc==0 && DAV_TRUNCATED(rn)){ rc=-1; if(newtoken&&tokcap)newtoken[0]=0;
        ESP_LOGW(TAG,"REPORT %s truncated at %d bytes -> failed (fall back to PROPFIND)",coll,rn); }
    fprintf(stderr,"[dav] REPORT %s tok=%s -> st=%d rn=%d rc=%d\n",coll,token&&token[0]?"incr":"full",st,rn,rc);
    free(buf);
    return rc;
}

/* ---------------- discovery ---------------- */
int dav_prop_href(const DavCtx*d,const char*path,const char*propOpen,
                  const char*extra_ns,char*out,int cap){
    if(out&&cap) out[0]=0;
    char body[512];
    int bl=snprintf(body,sizeof body,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\" %s><d:prop>%s</d:prop></d:propfind>",
        extra_ns?extra_ns:"", propOpen);
    char url[640]; snprintf(url,sizeof url,"%s%s",d->base,path);
    char buf[4096]; int rn=0;
    int st=davreq(d,HTTP_METHOD_PROPFIND,url,0,"application/xml",NULL,body,bl,
                  buf,sizeof buf,&rn, NULL,0, NULL,0);
    dav_last_status = st<0?0:st;
    if(st<0) return -1;
    return dav_parse_prop_href(buf,propOpen,out,cap);
}

int dav_effective_host(const DavCtx*d,const char*path,char*out,int cap){
    if(out&&cap) out[0]=0;
    char body[256];
    int bl=snprintf(body,sizeof body,
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:current-user-principal/></d:prop></d:propfind>");
    char url[640]; snprintf(url,sizeof url,"%s%s",d->base,path);
    char eff[512]=""; char buf[2048]; int rn=0;
    int st=davreq(d,HTTP_METHOD_PROPFIND,url,0,"application/xml",NULL,body,bl,
                  buf,sizeof buf,&rn, NULL,0, eff,sizeof eff);
    if(st<0) return -1;
    const char*p=strstr(eff,"://"); if(!p) return -1;
    const char*slash=strchr(p+3,'/'); int n = slash ? (int)(slash-eff) : (int)strlen(eff);
    if(n>cap-1) n=cap-1;
    memcpy(out,eff,n); out[n]=0;
    return 0;
}

int dav_list_collections(const DavCtx*d,const char*path,dav_coll_cb cb,void*ctx){
    char body[256];
    int bl=snprintf(body,sizeof body,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:displayname/></d:prop></d:propfind>");
    char url[640]; snprintf(url,sizeof url,"%s%s",d->base,path);
    char*buf=malloc(DAV_LIST_CAP); if(!buf) return -1;
    int rn=0;
    int st=davreq(d,HTTP_METHOD_PROPFIND,url,1,"application/xml",NULL,body,bl,
                  buf,DAV_LIST_CAP,&rn, NULL,0, NULL,0);
    int count = st<0 ? -1 : dav_parse_collections(buf,cb,ctx);
    free(buf);
    return count;
}
