#!/usr/bin/env bash
# nodelete.sh -- nothing that ships may be able to delete from the server.
#
# The rule (bridge/dav.h): the bridge and the device never remove an object
# from a CalDAV/CardDAV account. There is no dav_delete, the device transport
# allow-lists its HTTP methods, and the host transport refuses a DELETE or MOVE
# command line. This scan keeps it that way: it fails if shipped code (bridge/,
# firmware/, sim/) names a DELETE or MOVE request or a dav_delete function.
# The gates' own cleanup of their local Radicale lives in tests/ and is not
# scanned. A line may opt out only with the marker `nodelete-ok` (the host
# transport's refusal is the one such line).
set -u
cd "$(dirname "$0")/.."

PAT='HTTP_METHOD_(DELETE|MOVE)|-X *(DELETE|MOVE)\b|"(DELETE|MOVE)"|\bdav_(delete|move|remove|rm)\b|request\s*=\s*.?(DELETE|MOVE)'

# the pattern itself must catch what it is for
for probe in 'davreq(d,HTTP_METHOD_DELETE,url' "curl -X DELETE '%s'" 'int dav_delete(' 'method = "MOVE";'; do
    printf '%s\n' "$probe" | grep -Eq "$PAT" || { echo "nodelete: pattern misses: $probe"; exit 1; }
done

hits=$(grep -rnE "$PAT" bridge firmware sim --include='*.c' --include='*.h' --include='*.cpp' --include='*.py' --include='*.sh' \
       --exclude-dir=lvgl --exclude-dir=managed_components --exclude-dir=build \
       | grep -v 'nodelete-ok' || true)
if [ -n "$hits" ]; then
    echo "nodelete: FAIL -- shipped code can delete from the server:"
    echo "$hits"
    exit 1
fi
echo "nodelete: no DELETE or MOVE request in shipped code"
