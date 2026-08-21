/* break_test.c -- offline gate for the DAV transport circuit breaker (bridge/dav_break.c).
 *
 * The rule this pins down is the difference between a sync that reports "server
 * unreachable" in a few seconds and one that spends forty minutes issuing the
 * same doomed request once per record. It is worth a gate precisely because the
 * failure it guards against is invisible in normal operation: you only find out
 * the breaker is wrong when the network is already broken.
 */
#include <stdio.h>
#include "../bridge/dav.h"

static int failures;
#define CHECK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); failures++; } }while(0)

int main(void){
    printf("DAV circuit breaker gate\n");

    dav_transport_reset();
    CHECK(!dav_transport_down(), "a freshly reset transport is up");

    /* One short of the limit must NOT trip: a couple of failed requests are
     * normal on a flaky link and must not cost the whole collection. */
    for(int i=0;i<DAV_FAIL_LIMIT-1;i++) dav_note_result(-1);
    CHECK(!dav_transport_down(), "DAV_FAIL_LIMIT-1 consecutive failures stay up");

    dav_note_result(-1);
    CHECK(dav_transport_down(), "the DAV_FAIL_LIMIT'th consecutive failure trips it");

    /* Staying down is the whole point -- further failures must not un-trip it. */
    dav_note_result(-1);
    CHECK(dav_transport_down(), "it stays down once tripped");

    dav_transport_reset();
    CHECK(!dav_transport_down(), "reset re-arms it");

    /* A reply from the server clears the run, INCLUDING an error reply: 401 and
     * 404 both mean the connection worked and the server answered. Treating them
     * as unreachable would abandon a sync over a wrong password or a stale href. */
    for(int i=0;i<DAV_FAIL_LIMIT-1;i++) dav_note_result(-1);
    dav_note_result(401);
    CHECK(!dav_transport_down(), "a 401 counts as reached and clears the run");
    for(int i=0;i<DAV_FAIL_LIMIT-1;i++) dav_note_result(-1);
    CHECK(!dav_transport_down(), "the run really restarted from zero after the 401");
    dav_note_result(404);
    for(int i=0;i<DAV_FAIL_LIMIT-1;i++) dav_note_result(-1);
    CHECK(!dav_transport_down(), "a 404 clears it too");

    /* Non-consecutive failures never accumulate to a trip. */
    dav_transport_reset();
    for(int i=0;i<DAV_FAIL_LIMIT*3;i++){ dav_note_result(-1); dav_note_result(207); }
    CHECK(!dav_transport_down(), "failures interleaved with replies never trip it");

    printf(failures ? "\nBreaker gate: %d FAIL\n" : "\nBreaker gate: OK\n", failures);
    return failures ? 1 : 0;
}
