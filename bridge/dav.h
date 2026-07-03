/* dav.h -- minimal CalDAV/CardDAV client used by the bridge.
 * Host build shells out to the `curl` binary; on ESP32 the same request
 * shapes (PUT / GET / PROPFIND Depth:1) map to esp_http_client + mbedTLS.
 */
#ifndef DAV_H
#define DAV_H

typedef struct { char base[256]; char user[64]; char pass[64]; } DavCtx;

/* PUT body (from file) to <base>/<coll>/<name>; captures returned ETag and
 * HTTP status. ifmatch: NULL/"" = unconditional; else sent as If-Match (a
 * 412 means the object changed on the server since ifmatch was observed).  */
int dav_put(const DavCtx*d,const char*coll,const char*name,const char*ctype,
            const char*bodyfile,const char*ifmatch,
            char*etag_out,int etagcap,int*status_out);

/* DELETE <base>/<coll>/<name>; ifmatch optional. returns HTTP status or -1. */
int dav_delete(const DavCtx*d,const char*coll,const char*name,const char*ifmatch);

/* PROPFIND Depth:0 for a single object's current ETag. returns 0 on success. */
int dav_getetag(const DavCtx*d,const char*coll,const char*name,char*etag,int cap);

/* GET <base>/<coll>/<name> into out buffer. returns bytes or -1. */
int dav_get(const DavCtx*d,const char*coll,const char*name,char*out,int cap);

/* PROPFIND Depth:1 on <base>/<coll>/ ; cb(name, etag) per member object. */
typedef void (*dav_list_cb)(const char*name,const char*etag,void*ctx);
int dav_list(const DavCtx*d,const char*coll,dav_list_cb cb,void*ctx);

#endif
