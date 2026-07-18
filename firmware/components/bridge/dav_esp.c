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
#include <unistd.h>          /* ftruncate for the per-attempt spool reset */
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "dav.h"
#include "dav_xml.h"

static const char *TAG = "dav";
int dav_last_status = 0;

/* accumulates the response body into a caller-owned buffer + grabs the ETag.
 * When `spool` is set, the body is instead streamed to that FILE with no size
 * cap (the caller stream-parses it) -- this is how large collection enumerations
 * avoid needing a full-response RAM buffer on the no-PSRAM device. */
typedef struct {
    char *buf; int len, cap; int truncated;
    FILE *spool;
    char etag[160];
} RespAcc;

/* --- persistent keep-alive connection ------------------------------------
 * Init/perform/cleanup per request meant a full TLS handshake (~1-3 s against
 * iCloud's cert chain) for EVERY DAV call -- ~2N handshakes to pull N records
 * (one GET per object for its UID, one more for its body, plus each PUT/DELETE).
 * We now keep ONE client handle alive and reuse its TCP+TLS connection
 * (keep_alive_enable) across calls to the same origin, so the handshake happens
 * about once per network phase instead of once per request.
 *
 * RAM stays bounded exactly as before: dav_disconnect() (called from the engine's
 * sortFile(), i.e. immediately before every heap-heavy disk sort, and at end of
 * sync) tears the ~40 KB TLS working set back down so it never coexists with a
 * sort. The handle is bound to one origin (scheme://host[:port]); a request to a
 * different origin transparently rebuilds it. s_acc carries the current call's
 * response accumulator to on_event (user_data can't, since the handle outlives
 * each call's stack frame). */
static esp_http_client_handle_t s_client;
static char s_origin[128];
static RespAcc *s_acc;
/* when set, davreq streams the response body to this FILE (see RespAcc.spool). */
static FILE *s_spoolfile;
/* SD spool for a collection enumeration response (REPORT / PROPFIND); parsed in a
 * sliding window so a large collection never needs a full-body RAM buffer. */
#define ENUM_SPOOL "/sdcard/state/.enum"

/* scheme://host[:port] prefix of an absolute URL. */
static void url_origin(const char*url, char*out, int cap){
    const char*p = strstr(url,"://");
    if(!p){ if(cap) out[0]=0; return; }
    const char*slash = strchr(p+3,'/');
    int n = slash ? (int)(slash-url) : (int)strlen(url);
    if(n>=cap) n=cap-1;
    if(n<0) n=0;
    memcpy(out,url,n); out[n]=0;
}

void dav_disconnect(void){
    if(s_client){ esp_http_client_cleanup(s_client); s_client=NULL; }
    s_origin[0]=0;
}

static esp_err_t on_event(esp_http_client_event_t *e){
    RespAcc *a = s_acc;
    if(!a) return ESP_OK;
    switch(e->event_id){
    case HTTP_EVENT_ON_HEADER:
        if(e->header_key && strcasecmp(e->header_key,"ETag")==0 && e->header_value)
            snprintf(a->etag,sizeof a->etag,"%s",e->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        if(a->spool){                          /* stream to SD, uncapped */
            if(e->data_len>0){ a->len += (int)fwrite(e->data,1,e->data_len,a->spool); }
        } else if(a->buf && a->cap>0){
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
    if(!d->user[0]){ if(cap) out[0]=0; return; }   /* public GET (empty user): no auth */
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
    RespAcc acc = { .buf=resp, .cap=respcap, .spool=s_spoolfile };
    s_acc = &acc;

    char origin[128]; url_origin(url,origin,sizeof origin);
    /* a request to a different origin than the live handle can't reuse it */
    if(s_client && strcmp(s_origin,origin)!=0) dav_disconnect();

    char auth[320]; basic_auth(d,auth,sizeof auth);
    int status = -1;

    /* Try on the reused connection; if perform fails (e.g. the server dropped an
     * idle keep-alive socket), drop the handle and retry once on a fresh one. */
    for(int attempt=0; attempt<2; attempt++){
        acc.len=0; acc.truncated=0; acc.etag[0]=0;
        if(resp && respcap) resp[0]=0;
        if(acc.spool){ rewind(acc.spool); ftruncate(fileno(acc.spool),0); }  /* fresh spool per attempt */

        if(!s_client){
            esp_http_client_config_t cfg = {
                .url = url,
                .method = method,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .event_handler = on_event,
                .timeout_ms = 20000,
                .disable_auto_redirect = false,
                .max_redirection_count = 5,
                .buffer_size = 2048,
                .buffer_size_tx = 2048,
                .keep_alive_enable = true,   /* reuse the TCP+TLS connection */
            };
            s_client = esp_http_client_init(&cfg);
            if(!s_client){ s_acc=NULL; return -1; }
            snprintf(s_origin,sizeof s_origin,"%s",origin);
        }
        esp_http_client_handle_t c = s_client;

        esp_http_client_set_url(c,url);
        esp_http_client_set_method(c,method);
        /* headers persist on a reused handle -> clear the ones that vary so, e.g.,
         * a prior PUT's If-Match can't leak into a later GET. */
        esp_http_client_delete_header(c,"Content-Type");
        esp_http_client_delete_header(c,"Depth");
        esp_http_client_delete_header(c,"If-Match");
        if(auth[0]) esp_http_client_set_header(c,"Authorization",auth);
        if(ctype)   esp_http_client_set_header(c,"Content-Type",ctype);
        if(depth>=0){ char dh[4]={ depth?'1':'0', 0 }; esp_http_client_set_header(c,"Depth",dh); }
        if(ifmatch && ifmatch[0]){ char v[176]; snprintf(v,sizeof v,"\"%s\"",ifmatch);
            esp_http_client_set_header(c,"If-Match",v); }
        esp_http_client_set_post_field(c, body?body:NULL, body?bodylen:0);

        esp_err_t err = esp_http_client_perform(c);
        if(err==ESP_OK){
            status = esp_http_client_get_status_code(c);
            if(effurl && effcap) esp_http_client_get_url(c, effurl, effcap);
            break;
        }
        /* failed: the connection may be stale -- tear it down and, on the first
         * attempt, retry fresh. A second failure is a real error. */
        ESP_LOGW(TAG,"%s failed: %s%s", url, esp_err_to_name(err),
                 attempt==0 ? " (retrying on a fresh connection)" : "");
        dav_disconnect();
    }

    if(etag && etagcap) snprintf(etag,etagcap,"%s",acc.etag);
    if(respn) *respn = acc.len;
    if(acc.truncated) ESP_LOGW(TAG,"response truncated at %d bytes: %s", respcap, url);
    s_acc = NULL;
    /* connection is left OPEN for the next request; dav_disconnect() (engine
     * sortFile / end of sync) frees it before the next heap-heavy sort. */
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

/* Stream an arbitrary (public) https URL's body straight to an SD file -- used by
 * the News reader's HotSync fetch to spool a feed for the sliding-window RSS
 * parser, exactly as dav_list spools a PROPFIND. No auth, no size cap in RAM.
 * Returns the HTTP status, or -1. Device-only (dav.h declares it; dav.c/host has
 * no implementation because the host never fetches feeds). */
int dav_fetch_url(const char *url, const char *path){
    FILE *sp = fopen(path,"w+b"); if(!sp) return -1;
    DavCtx pub; memset(&pub,0,sizeof pub);      /* empty user => no Authorization */
    s_spoolfile = sp;
    int st = davreq(&pub,HTTP_METHOD_GET,url,-1,NULL,NULL,NULL,0, NULL,0,NULL, NULL,0, NULL,0);
    s_spoolfile = NULL;
    fclose(sp);
    return st;
}

/* response buffer for listings; heap (freed per call) so it doesn't compete with
 * the ~40KB TLS handshake for long. */

int dav_list(const DavCtx*d,const char*coll,dav_list_cb cb,void*ctx){
    char url[512]; snprintf(url,sizeof url,"%s/%s/",d->base,coll);
    char body[128]; int bl=body_getetag(body,sizeof body);
    /* Spool the (possibly large) PROPFIND response to SD and parse it in a sliding
     * window -- no full-collection RAM buffer, so the member list has no size cap. */
    FILE*sp=fopen(ENUM_SPOOL,"w+b"); if(!sp){ ESP_LOGW(TAG,"PROPFIND %s: spool open failed",coll); return -1; }
    s_spoolfile=sp;
    int st=davreq(d,HTTP_METHOD_PROPFIND,url,1,"application/xml",NULL,body,bl,
                  NULL,0,NULL, NULL,0, NULL,0);
    s_spoolfile=NULL;
    long rn = ftell(sp); rewind(sp);
    int count = st<0 ? -1 : dav_parse_members_stream(sp,cb,ctx);
    fclose(sp); remove(ENUM_SPOOL);
    fprintf(stderr,"[dav] PROPFIND %s -> st=%d rn=%ld members=%d (stream)\n",coll,st,rn,count);
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
    /* Spool the REPORT reply to SD and stream-parse it: the etag list + trailing
     * sync-token can be far larger than any RAM buffer, and truncating would lose
     * records + the token. Streaming removes the 8 KB enumeration cap entirely. */
    FILE*sp=fopen(ENUM_SPOOL,"w+b"); if(!sp){ ESP_LOGW(TAG,"REPORT %s: spool open failed",coll); return -1; }
    s_spoolfile=sp;
    int st=davreq(d,HTTP_METHOD_REPORT,url,1,"application/xml",NULL,body,bl,
                  NULL,0,NULL, NULL,0, NULL,0);
    s_spoolfile=NULL;
    long rn = ftell(sp); rewind(sp);
    int rc = st<0 ? -1 : dav_parse_report_stream(sp,st,cb,ctx,newtoken,tokcap);
    fclose(sp); remove(ENUM_SPOOL);
    fprintf(stderr,"[dav] REPORT %s tok=%s -> st=%d rn=%ld rc=%d (stream)\n",coll,token&&token[0]?"incr":"full",st,rn,rc);
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
    /* Spool the home-set PROPFIND to SD and stream-parse it: an iCloud calendar
     * home returns calendars + reminder lists + inbox/outbox/notification, which
     * together overrun any small RAM buffer -- and the reminder lists trail the
     * calendars, so a truncated buffer would silently drop them from discovery. */
    FILE*sp=fopen(ENUM_SPOOL,"w+b"); if(!sp){ ESP_LOGW(TAG,"PROPFIND collections: spool open failed"); return -1; }
    s_spoolfile=sp;
    int st=davreq(d,HTTP_METHOD_PROPFIND,url,1,"application/xml",NULL,body,bl,
                  NULL,0,NULL, NULL,0, NULL,0);
    s_spoolfile=NULL;
    long rn = ftell(sp); rewind(sp);
    int count = st<0 ? -1 : dav_parse_collections_stream(sp,cb,ctx);
    fclose(sp); remove(ENUM_SPOOL);
    fprintf(stderr,"[dav] PROPFIND collections %s -> st=%d rn=%ld found=%d (stream)\n",path,st,rn,count);
    return count;
}
