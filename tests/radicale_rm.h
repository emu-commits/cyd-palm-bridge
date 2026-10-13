/* radicale_rm.h -- empty the gates' throwaway Radicale between scenarios.
 *
 * The bridge itself has no way to delete anything on a server, and never will
 * (bridge/dav.h). The gates still need a clean collection to start from, and
 * to play "deleted on the server" behind the engine's back, so they remove
 * objects here, with curl, outside the bridge. It refuses any server that
 * isn't on this machine: a gate pointed at a real account must fail, not
 * empty it. */
#ifndef RADICALE_RM_H
#define RADICALE_RM_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bridge/dav.h"

static int test_is_local(const DavCtx *d){
    return !strncmp(d->base, "http://localhost:", 17) || !strncmp(d->base, "http://127.0.0.1:", 17);
}
/* Remove <base>/<coll>/<name> from the LOCAL test server. */
static void test_rm(const DavCtx *d, const char *coll, const char *name, const char *unused){
    (void)unused;
    if(!test_is_local(d)){
        fprintf(stderr, "test_rm: refusing to delete on %s (not a local test server)\n", d->base);
        exit(2);
    }
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "curl -s -o /dev/null -u %s:%s -X DELETE '%s/%s/%s'",
             d->user, d->pass, d->base, coll, name);
    if(system(cmd)){}
}
#endif
