#!/usr/bin/env bash
# End-to-end: PDB -> real DAV server (Radicale) -> PDB, then diff.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p pdb state

BASE=${DAV_BASE:-http://localhost:5232}
AUTH=palm:palm
CAL=palm/cal
CARD=palm/card

echo "== regenerate source PDBs =="
./roundtrip >/dev/null
ls -l pdb/DatebookDB.pdb pdb/AddressDB.pdb

echo "== reset server collections + local state =="
# delete any existing objects so the run is clean
for coll in $CAL $CARD; do
  for href in $(curl -s -u $AUTH -X PROPFIND -H 'Depth: 1' "$BASE/$coll/" \
      | grep -oE '<D:href>[^<]+</D:href>|<href>[^<]+</href>' | sed -E 's/<[^>]+>//g' | grep -E '\.(ics|vcf)$' || true); do
    curl -s -o /dev/null -u $AUTH -X DELETE "$BASE$href" || true
  done
done
rm -f state/sync_map.tsv

echo "== push =="
./bridge_cli push pdb/DatebookDB.pdb pdb/AddressDB.pdb

echo "== server now holds (sample GET of 1.ics) =="
curl -s -u $AUTH "$BASE/$CAL/1.ics" | sed 's/\r$//'

echo "== sync map written =="
cat state/sync_map.tsv

echo "== pull into fresh PDBs =="
./bridge_cli pull pdb/DatebookDB.down.pdb pdb/AddressDB.down.pdb

echo "== diff canonical dumps (original vs server-rebuilt) =="
./bridge_cli dump cal  pdb/DatebookDB.pdb      > state/cal.orig.txt
./bridge_cli dump cal  pdb/DatebookDB.down.pdb  > state/cal.down.txt
./bridge_cli dump card pdb/AddressDB.pdb        > state/card.orig.txt
./bridge_cli dump card pdb/AddressDB.down.pdb   > state/card.down.txt

rc=0
# CAL preserves order (calendars have no slot ambiguity) -> exact diff.
if diff -u state/cal.orig.txt state/cal.down.txt >/dev/null; then
  echo "CAL  round-trip: IDENTICAL (exact)"
else
  echo "CAL  round-trip: DIFF"; diff -u state/cal.orig.txt state/cal.down.txt; rc=1
fi
# CARD: vCard has no Palm phone-slot order, and Radicale reorders properties,
# so assert set-equality of all emitted properties (no data lost/changed).
if diff -u <(sort state/card.orig.txt) <(sort state/card.down.txt) >/dev/null; then
  echo "CARD round-trip: IDENTICAL (property set; slot order not preserved by vCard)"
else
  echo "CARD round-trip: DIFF (real data loss)"; diff -u <(sort state/card.orig.txt) <(sort state/card.down.txt); rc=1
fi
exit $rc
