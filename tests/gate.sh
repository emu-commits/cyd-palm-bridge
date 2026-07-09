#!/usr/bin/env bash
# gate.sh -- run one-or-more server gates against a throwaway Radicale, reliably.
# Unlike run_gates.sh this VERIFIES the collections are writable (retry until a
# real PUT succeeds) before running anything, which the sandbox needs.
#   ./tests/gate.sh incremental bigsync multiapp   # run these gates
#   ./tests/gate.sh                                 # run the full server set
set -u
cd "$(dirname "$0")/.."
PORT=5232; BASE="http://localhost:$PORT"; U=palm; P=palm
PY=./davvenv/bin/python
mkdir -p pdb state
printf '%s:%s\n' "$U" "$P" > radicale.htpasswd
rm -rf radicale-collections
"$PY" -m radicale --config radicale.conf >state/radicale.log 2>&1 &
RPID=$!
cleanup(){ kill "$RPID" 2>/dev/null; wait "$RPID" 2>/dev/null; }
trap cleanup EXIT

mkcolls(){
  curl -s -o /dev/null -u "$U:$P" -X MKCALENDAR "$BASE/palm/cal/"
  curl -s -o /dev/null -u "$U:$P" -X MKCALENDAR "$BASE/palm/todo/"
  curl -s -o /dev/null -u "$U:$P" -X MKCOL -H 'Content-Type: application/xml' \
    --data '<D:mkcol xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav"><D:set><D:prop><D:resourcetype><D:collection/><C:addressbook/></D:resourcetype></D:prop></D:set></D:mkcol>' \
    "$BASE/palm/card/"
}
# wait until a real PUT succeeds (proves storage + collection are live)
ok=0
for i in $(seq 1 40); do
  kill -0 "$RPID" 2>/dev/null || { echo "radicale died"; tail -20 state/radicale.log; exit 2; }
  mkcolls
  st=$(curl -s -o /dev/null -w '%{http_code}' -u "$U:$P" -X PUT \
        -H 'Content-Type: text/calendar' \
        --data-binary $'BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:probe@cyd\r\nDTSTART:20260101T000000\r\nDTEND:20260101T010000\r\nSUMMARY:probe\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n' \
        "$BASE/palm/cal/probe.ics")
  if [ "$st" = 201 ] || [ "$st" = 204 ]; then
    curl -s -o /dev/null -u "$U:$P" -X DELETE "$BASE/palm/cal/probe.ics"; ok=1; break
  fi
  sleep 0.3
done
[ "$ok" = 1 ] || { echo "collections never became writable (last PUT=$st)"; tail -20 state/radicale.log; exit 2; }

make >/dev/null || { echo "build failed"; exit 1; }
GATES="${*:-incremental synctoken category uidmatch bigsync multiapp}"
rc=0
for g in $GATES; do
  rm -rf state; mkdir -p state
  # collections were wiped with state? no -- radicale-collections is separate. keep them.
  echo "===== $g ====="
  if ./"$g"; then echo "  -> PASS"; else echo "  -> FAIL"; rc=1; fi
done
echo; [ "$rc" = 0 ] && echo "GREEN" || echo "FAILED"
exit $rc
