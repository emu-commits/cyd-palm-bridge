/* dav.h -- the HTTPS transport HotSync's fetches use (news, weather,
 * location), and its circuit breaker.
 *
 * It was the CalDAV/CardDAV client for the iCloud sync until that was archived
 * on 2026-11-07 (git tag archive/pim-caldav). The device implementation is
 * firmware/components/bridge/dav_esp.c; the breaker is bridge/dav_break.c,
 * shared with the host's break_test.
 */
#ifndef DAV_H
#define DAV_H

typedef struct { char base[256]; char user[128]; char pass[64]; } DavCtx;

extern int dav_last_status;   /* HTTP status of the most recent request that set it */

/* GET a public https URL, the body streamed to the file at `path` (no RAM cap).
 * Returns the HTTP status, or -1. Device only. */
int dav_fetch_url(const char *url, const char *path);

/* Close the persistent keep-alive connection, freeing the ~40 KB TLS working
 * set. HotSync calls it between phases and before Wi-Fi goes down. */
void dav_disconnect(void);

/* ---- transport circuit breaker --------------------------------------------
 * A request that cannot reach the server costs up to two 20-second timeouts.
 * So the transport counts consecutive unreachable requests. Once DAV_FAIL_LIMIT
 * of them go by, it is considered DOWN and every later request returns -1
 * immediately without touching the network. Any reply from the server clears
 * the count, including 401 or 404: those mean the connection worked.
 *
 * dav_transport_reset() re-arms it; call at the start of each phase so one bad
 * fetch cannot condemn the next. dav_transport_down() reports the state. */
/* Heap-integrity checkpoint hook, installed by the device's HotSync. */
typedef void (*DavCheckFn)(const char *where);
void dav_set_check(DavCheckFn fn);
void dav_check(const char *where);

#define DAV_FAIL_LIMIT 3
void dav_transport_reset(void);
int  dav_transport_down(void);
/* Every completed request reports its outcome here. status < 0 means the request
 * never reached the server (DNS, TCP or TLS); any HTTP status means it did. */
void dav_note_result(int status);

#endif
