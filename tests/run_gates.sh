#!/usr/bin/env bash
# run_gates.sh -- bring up a throwaway Radicale and run every host gate in one shot.
#
# The host bridge is fully exercisable with nothing but a C compiler and a local
# Radicale (localhost, no external network). This script owns the whole loop:
# start Radicale, create the calendar + address-book collections the tests need,
# run all gates from a clean state/ dir, then tear Radicale down.
#
#   ./tests/run_gates.sh            # build + run every gate
#   ./tests/run_gates.sh -k         # keep Radicale running after (for manual poking)
#
# Exits non-zero if any gate fails. Radicale is found via ./davvenv (the README's
# venv) if present, else a system `python3 -m radicale`.
set -u
cd "$(dirname "$0")/.."

KEEP=0
[ "${1:-}" = "-k" ] && KEEP=1

PORT=5232
BASE="http://localhost:$PORT"
U=palm; P=palm

# --- pick a radicale ---
if [ -x ./davvenv/bin/python ] && ./davvenv/bin/python -c 'import radicale' 2>/dev/null; then
    PY=./davvenv/bin/python
elif python3 -c 'import radicale' 2>/dev/null; then
    PY=python3
else
    echo "ERROR: radicale not found. Install it with one of:"
    echo "  python3 -m venv davvenv && ./davvenv/bin/pip install radicale"
    echo "  pip3 install radicale"
    exit 2
fi

mkdir -p pdb state
printf '%s:%s\n' "$U" "$P" > radicale.htpasswd

echo "== starting Radicale ($PY) on :$PORT =="
rm -rf radicale-collections
"$PY" -m radicale --config radicale.conf >state/radicale.log 2>&1 &
RPID=$!
cleanup(){ [ "$KEEP" = 1 ] || { kill "$RPID" 2>/dev/null; wait "$RPID" 2>/dev/null; }; }
trap cleanup EXIT

# wait for it to accept connections
for i in $(seq 1 30); do
    if curl -s -o /dev/null -u "$U:$P" "$BASE/"; then break; fi
    if ! kill -0 "$RPID" 2>/dev/null; then echo "Radicale died:"; tail -20 state/radicale.log; exit 2; fi
    sleep 0.3
done

# collections the gates expect (idempotent; ignore already-exists errors)
curl -s -o /dev/null -u "$U:$P" -X MKCALENDAR "$BASE/palm/cal/"
curl -s -o /dev/null -u "$U:$P" -X MKCOL -H 'Content-Type: application/xml' \
  --data '<D:mkcol xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav"><D:set><D:prop><D:resourcetype><D:collection/><C:addressbook/></D:resourcetype></D:prop></D:set></D:mkcol>' \
  "$BASE/palm/card/"

echo "== building =="
make >/dev/null || { echo "build failed"; exit 1; }

rc=0
run(){   # run <label> <clean-state?> <command...>
    local label="$1" clean="$2"; shift 2
    [ "$clean" = clean ] && { rm -rf state; mkdir -p state; }
    echo "== $label =="
    if "$@"; then echo "  -> PASS"; else echo "  -> FAIL"; rc=1; fi
}

run "roundtrip (codec, offline)" noclean   ./roundtrip
run "find (global search, offline)" noclean ./find_test
run "calc (evaluator, offline)"    noclean  ./calc_test
run "config (prefs, offline)"      noclean  ./config_test
run "fuzz (parser hardening, ASan)" noclean ./fuzz_test
run "incremental (two-way + policies)" clean ./incremental
run "synctoken (RFC 6578 delta)"       clean ./synctoken
run "category (category->collection)"  clean ./category
run "bigsync (device-sized, 90 recs)"  clean ./bigsync
run "multiapp (To Do + Address sync)"  clean ./multiapp
run "dav_roundtrip (PDB->server->PDB)" clean ./tests/dav_roundtrip.sh

echo
if [ "$rc" = 0 ]; then echo "ALL GATES GREEN"; else echo "SOME GATES FAILED"; fi
exit $rc
