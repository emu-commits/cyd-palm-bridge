/* dav_break.c -- the DAV transport's circuit breaker (contract in dav.h).
 *
 * Portable and shared by both transports, so the rule lives in one place and the
 * host gate can drive it directly rather than having to stage a real network
 * failure. The device transport (dav_esp.c) is what wires it in: it asks
 * dav_transport_down() before spending a timeout and reports every outcome to
 * dav_note_result(). The host transport is a fresh curl process per call with
 * its own timeouts and no long-lived TLS handle to starve, so it does not arm
 * the breaker; the logic is still gate-tested here.
 */
#include "dav.h"

/* Consecutive requests that never reached the server. Not the same as requests
 * that FAILED: a 401 or a 404 is the server answering, and clears this. */
static int s_fail_run;

void dav_transport_reset(void){ s_fail_run = 0; }
int  dav_transport_down(void){ return s_fail_run >= DAV_FAIL_LIMIT; }

void dav_note_result(int status){
    if(status < 0) s_fail_run++;
    else           s_fail_run = 0;
}

/* ---- heap-integrity checkpoints (temporary; contract in dav.h) ---- */
static DavCheckFn s_check;
void dav_set_check(DavCheckFn fn){ s_check = fn; }
void dav_check(const char *where){ if(s_check) s_check(where); }
