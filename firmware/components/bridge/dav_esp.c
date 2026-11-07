/* dav_esp.c -- the device's HTTPS transport (ESP32, mbedTLS), for HotSync's
 * fetches: the news feeds, the weather and the location lookup, each spooled
 * to a file on the card (dav_fetch_url).
 *
 * It began as the CalDAV/CardDAV transport for the iCloud sync, which was
 * archived on 2026-11-07 (git tag archive/pim-caldav, with the sync engine
 * and the DAV requests). What is left is the part the fetches need: one
 * keep-alive esp_http_client handle (TLS via the Mozilla cert bundle,
 * chunked decode and redirects handled by the client), the body spooled to
 * SD, and the circuit breaker in dav_break.c. The names keep their dav_
 * prefix so the history reads straight.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>          /* ftruncate for the per-attempt spool reset */
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "dav.h"

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
 * Init/perform/cleanup per request meant a full TLS handshake (~1-3 s) for
 * every request. ONE client handle is kept alive and its TCP+TLS connection
 * reused (keep_alive_enable) across calls to the same origin, so the
 * handshake happens about once per origin instead of once per request.
 *
 * RAM stays bounded: dav_disconnect() (HotSync calls it between phases and
 * before Wi-Fi goes down) tears the ~40 KB TLS working set back down. The handle is bound to one origin (scheme://host[:port]); a request to a
 * different origin transparently rebuilds it. s_acc carries the current call's
 * response accumulator to on_event (user_data can't, since the handle outlives
 * each call's stack frame). */
static esp_http_client_handle_t s_client;
static char s_origin[128];
static RespAcc *s_acc;
/* when set, davreq streams the response body to this FILE (see RespAcc.spool). */
static FILE *s_spoolfile;

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

/* One request. `url` is absolute (scheme://host[:port]/path). body may be NULL.
 * depth: 0/1 => send that Depth header; <0 => none. Returns HTTP status or -1.
 * On success, resp holds the (truncated-to-cap) body, *respn its length, etag the
 * response ETag, effurl the final URL after any redirects. */
static int davreq(const DavCtx*d, esp_http_client_method_t method, const char*url,
                  int depth, const char*ctype, const char*ifmatch,
                  const char*body, int bodylen,
                  char*resp, int respcap, int*respn,
                  char*etag, int etagcap, char*effurl, int effcap){
    /* An allow-list: GET is all HotSync's fetches need, and nothing else
     * leaves this device. */
    if(method!=HTTP_METHOD_GET){
        if(resp && respcap) resp[0]=0;
        if(respn) *respn = 0;
        if(etag && etagcap) etag[0]=0;
        return -1;
    }
    /* Breaker first: once the transport is down, spending another 20-second
     * timeout tells us nothing we do not already know, and there is one request
     * per record behind this. */
    if(dav_transport_down()){
        if(resp && respcap) resp[0]=0;
        if(respn) *respn = 0;
        if(etag && etagcap) etag[0]=0;
        return -1;
    }

    RespAcc acc = { .buf=resp, .cap=respcap, .spool=s_spoolfile };
    s_acc = &acc;

    char origin[128]; url_origin(url,origin,sizeof origin);
    /* a request to a different origin than the live handle can't reuse it */
    if(s_client && strcmp(s_origin,origin)!=0) dav_disconnect();

    (void)d;                                  /* public fetches: no Authorization */
    int status = -1;

    /* Try on the reused connection; if perform fails (e.g. the server dropped an
     * idle keep-alive socket), drop the handle and retry once on a fresh one. */
    for(int attempt=0; attempt<2; attempt++){
        /* Whether there is a connection to blame. The retry below exists for one
         * case only: a keep-alive socket the server closed while it sat idle. If
         * we are opening a fresh connection anyway, a second identical attempt
         * just spends another 20 seconds arriving at the same answer. */
        int had_reuse = (s_client != NULL);
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
                 (attempt==0 && had_reuse) ? " (retrying on a fresh connection)" : "");
        dav_disconnect();
        if(!had_reuse) break;          /* nothing stale to blame -- do not pay twice */
    }

    dav_note_result(status);
    if(status < 0 && dav_transport_down())
        ESP_LOGE(TAG,"transport down after %d consecutive unreachable requests -- "
                     "abandoning the rest of this phase", DAV_FAIL_LIMIT);
    if(etag && etagcap) snprintf(etag,etagcap,"%s",acc.etag);
    if(respn) *respn = acc.len;
    if(acc.truncated) ESP_LOGW(TAG,"response truncated at %d bytes: %s", respcap, url);
    s_acc = NULL;
    /* connection is left OPEN for the next request; dav_disconnect() frees it */
    return status;
}

/* Stream a public https URL's body straight to an SD file -- a feed for the
 * sliding-window RSS parser, the weather CSV, the location reply. No auth, no
 * size cap in RAM. Returns the HTTP status, or -1. Device-only. */
int dav_fetch_url(const char *url, const char *path){
    FILE *sp = fopen(path,"w+b"); if(!sp) return -1;
    DavCtx pub; memset(&pub,0,sizeof pub);      /* empty user => no Authorization */
    s_spoolfile = sp;
    int st = davreq(&pub,HTTP_METHOD_GET,url,-1,NULL,NULL,NULL,0, NULL,0,NULL, NULL,0, NULL,0);
    s_spoolfile = NULL;
    fclose(sp);
    return st;
}

