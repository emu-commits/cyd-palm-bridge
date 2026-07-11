/* dav_xml.h -- transport-independent DAV response parsing.
 *
 * The XML scanning and the multistatus <response> walks are identical whether
 * the bytes arrived via the curl binary (host, dav.c) or mbedTLS (device,
 * dav_esp.c). Both transports fetch a response body, then hand it here. Keeping
 * one copy means fixes like the collection-self filter live in a single place.
 */
#ifndef DAV_XML_H
#define DAV_XML_H
#include <stdio.h>  /* FILE for the streaming parsers */
#include "dav.h"   /* dav_list_cb / dav_sync_cb / dav_coll_cb typedefs */

/* ---- leaf helpers (namespace/attribute tolerant, optionally bounded) ---- */
/* case-insensitive substring search bounded by [s,end). */
const char* dav_strcasestr_range(const char*s,const char*end,const char*needle);
/* content start (after '>') of the first element with local-name `name` in
 * [from,end); end may be NULL for unbounded. NULL if not found. */
const char* dav_xml_open(const char*from,const char*end,const char*name);
/* trimmed text content of element `name` in [from,end) into out. 1 if found. */
int dav_xml_text(const char*from,const char*end,const char*name,char*out,int cap);
/* strip surrounding quotes and a leading weak-etag "W/" marker, in place. */
void dav_strip_quotes(char*s);
/* last path segment of `full` (ignoring trailing slashes) into out. */
void dav_basename(const char*full,char*out,int cap);
/* nonzero if href is a collection self-entry (ends in '/'), not a member. */
int dav_href_is_coll(const char*h);

/* ---- high-level parsers over a NUL-terminated response body ---- */
/* walk a PROPFIND multistatus: cb(name basename, etag) per member. count. */
int dav_parse_members(const char*buf,dav_list_cb cb,void*ctx);
/* walk a sync-collection REPORT reply. `status` is the HTTP code.
 * returns 0 ok (and fills newtoken), 1 token-expired, -1 unsupported. */
int dav_parse_report(const char*buf,int status,dav_sync_cb cb,void*ctx,
                     char*newtoken,int tokcap);
/* ---- streaming parsers over a spooled response FILE (no full-body buffer) ----
 * Identical semantics to the buffer parsers above, but read the response from an
 * open FILE in a sliding window so a large collection's member/etag list never
 * needs to fit in RAM (the no-PSRAM device spools the response to SD). The FILE
 * must be positioned at the start of the response body. */
int dav_parse_members_stream(FILE*f,dav_list_cb cb,void*ctx);
int dav_parse_report_stream(FILE*f,int status,dav_sync_cb cb,void*ctx,
                            char*newtoken,int tokcap);

/* walk a PROPFIND with resourcetype/displayname: cb(href, kind, dn). count. */
int dav_parse_collections(const char*buf,dav_coll_cb cb,void*ctx);
/* pull the inner <href> of the element named by `propOpen` (e.g.
 * "<d:current-user-principal/>"). 0 on success. */
int dav_parse_prop_href(const char*buf,const char*propOpen,char*out,int cap);

#endif
