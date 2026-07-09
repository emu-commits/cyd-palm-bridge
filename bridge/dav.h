/* dav.h -- minimal CalDAV/CardDAV client used by the bridge.
 * Host build shells out to the `curl` binary; on ESP32 the same request
 * shapes (PUT / GET / PROPFIND Depth:1) map to esp_http_client + mbedTLS.
 */
#ifndef DAV_H
#define DAV_H

typedef struct { char base[256]; char user[128]; char pass[64]; } DavCtx;

extern int dav_last_status;   /* HTTP status of the most recent discovery PROPFIND */

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

/* RFC 6578 sync-collection REPORT. token="" for an initial (full) sync; else
 * the token stored from the previous run. cb fires once per changed member
 * (deleted=1 => removed on server). newtoken receives the server's fresh token.
 * returns: 0 ok; 1 = token invalid/expired (caller should full-resync with "");
 *          -1 = server doesn't support sync-collection (caller: fall back to PROPFIND). */
typedef void (*dav_sync_cb)(const char*name,const char*etag,int deleted,void*ctx);
int dav_sync_report(const DavCtx*d,const char*coll,const char*token,
                    dav_sync_cb cb,void*ctx,char*newtoken,int tokcap);

/* --- discovery (CalDAV/CardDAV bootstrap, e.g. iCloud) --- */
/* PROPFIND Depth:0 on `path`; returns the first <href> found inside the element
 * whose local-name is `prop` (e.g. "current-user-principal","calendar-home-set").
 * `extra_ns` is any additional xmlns decls the prop needs. 0 on success.        */
int dav_prop_href(const DavCtx*d,const char*path,const char*propOpen,
                  const char*extra_ns,char*out,int cap);
/* Effective URL (after redirects) for a PROPFIND on `path` -> scheme://host. */
int dav_effective_host(const DavCtx*d,const char*path,char*out,int cap);
/* PROPFIND Depth:1 on `path`; cb(href, kind) where kind: 'c'=calendar,
 * 'a'=addressbook, 0=other. href is the absolute path as returned.             */
typedef void (*dav_coll_cb)(const char*href,int kind,const char*displayname,void*ctx);
int dav_list_collections(const DavCtx*d,const char*path,dav_coll_cb cb,void*ctx);

#endif
