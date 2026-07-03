# CYD Palm→CalDAV/CardDAV bridge

Goal: a **native ESP32 PDA on the base CYD (`ESP32-2432S028R`, NO PSRAM, 4 MB flash,
~300 KB usable SRAM)** that reads Palm PIM databases and two-way syncs to
CalDAV/CardDAV over WiFi. Reuse PumpkinOS as a *donor codebase*, not a runtime.

## STATUS: end-to-end round trip PROVEN (host build)

The headless bridge is built and **verified against a real DAV server** (Radicale):
`Palm PDB <-> CalDAV/CardDAV` incremental two-way sync with conflict resolution.
Three test gates, all green:

- `make test` → `tests/roundtrip.c`: **254 checks, 0 failures.** PDB codec is lossless
  on every field; the full write→read→emit→parse→repack→reread chain reconstructs
  the originals — now including **VALARM, EXDATE, timezone conversion, and CP1252↔UTF-8**.
- `tests/dav_roundtrip.sh`: pushes real PDBs to Radicale, pulls them back into fresh
  PDBs, diffs canonical dumps. **CAL: byte-identical. CARD: property-set identical**
  (vCard doesn't preserve Palm's ordered phone slots; no data is lost/changed).
- `make itest` → `tests/incremental.c`: seeds a collection, makes divergent edits on
  **both** sides (local modify/delete/add + server modify/add + a genuine both-sides
  conflict), syncs under each policy, and asserts **local and server converge** to the
  expected set and that a **second sync is a total no-op (idempotent)**. All three
  policies (server-wins / local-wins / keep-both) pass.

### Layout
```
bridge/palm.h              shared model + module seams (no full DB ever resident)
bridge/pdb.c               PDB container: streaming reader (1 record RAM) + writer
bridge/datebook.c          Appt <-> bytes  (Pack + Unpack; PumpkinOS exceptions bug fixed)
bridge/address.c           Addr <-> bytes  (email-is-a-phone-slot quirk both ways)
bridge/ical.c              Appt <-> VEVENT (emit + the NEW parse/download half)
bridge/vcard.c             Addr <-> VCARD  (emit + parse; UID required by CardDAV)
bridge/tz.[ch]             timezone registry + DST math + UTC->local + VTIMEZONE emission
bridge/charset.[ch]        Palm CP1252/Latin-1 <-> UTF-8
bridge/dav.[ch]            CalDAV/CardDAV via curl (PUT+If-Match/GET/PROPFIND/DELETE) -> esp_http_client on device
bridge/sync.[ch]           push/pull primitives + sync_collection: incremental, conflict-aware two-way sync
bridge/main.c              CLI: push | pull | sync | dump   (BRIDGE_TZ env sets device zone)
tests/                     roundtrip.c (codec) + dav_roundtrip.sh (server) + incremental.c (two-way sync)
```

### First-time DAV server setup (for the server-backed tests)
```
python3 -m venv davvenv && ./davvenv/bin/pip install radicale
printf 'palm:palm\n' > radicale.htpasswd
./davvenv/bin/python -m radicale --config radicale.conf &        # localhost:5232
# create the two collections once:
curl -s -u palm:palm -X MKCALENDAR http://localhost:5232/palm/cal/
curl -s -u palm:palm -X MKCOL -H 'Content-Type: application/xml' \
  --data '<D:mkcol xmlns:D="DAV:" xmlns:C="urn:ietf:params:xml:ns:carddav"><D:set><D:prop>\
<D:resourcetype><D:collection/><C:addressbook/></D:resourcetype></D:prop></D:set></D:mkcol>' \
  http://localhost:5232/palm/card/
```

### Run it
```
make                       # builds ./roundtrip, ./bridge_cli, ./incremental
make test                  # codec round-trip (no server)
# real-server tests (Radicale in ./davvenv, config radicale.conf, creds palm:palm):
./davvenv/bin/python -m radicale --config radicale.conf &   # localhost:5232
./tests/dav_roundtrip.sh   # full PDB->server->PDB proof
make itest                 # incremental two-way sync + conflict policies + idempotence
# manual: DAV_BASE/DAV_USER/DAV_PASS/DAV_CAL/DAV_CARD env override the target
./bridge_cli push pdb/DatebookDB.pdb pdb/AddressDB.pdb          # full seed
./bridge_cli sync pdb/DatebookDB.pdb pdb/AddressDB.pdb server   # incremental (policy: server|local|both)
```

### Incremental sync (bridge/sync.c :: sync_collection)
Change detection: **local** = record's canonical-body FNV hash differs from the hash
stored in the map at last sync (or the Palm delete bit is set); **server** = object
ETag differs from the map's stored ETag (or it vanished / appeared). Reconciliation
runs the full (local-state × server-state) matrix — new/mod/del on each side — does the
DAV ops (conditional PUT with `If-Match`, DELETE, GET), writes the merged PDB, and
rewrites the map (`state/<coll>.map`, rows `uid⇥href⇥etag⇥hash`). A **conflict** is a
record changed on both sides; policy resolves it: `server` (server wins), `local` (local
wins), `both` (Palm-style keep-both — server copy stays, local is re-added as a new
record). Modify beats delete under `both`.

### What the real server taught us (not visible in a mock)
- Radicale **validates + normalizes** objects: it injected `DTSTAMP` into events and
  **rejects vCards without a UID** (silent empty-ETag) — fixed by emitting `UID:`.
- Servers return PROPFIND members **unordered**; pull sorts by uniqueID for a
  deterministic PDB.
- vCard is a *set* of typed values, not Palm's fixed 5 ordered phone slots, so slot
  index is not preserved across a round trip (data is). `displayPhone` isn't carried.

### Data fidelity (all closed)
- **Timezones** — `BRIDGE_TZ` sets the device zone (registry in `tz.c`: US + EU zones,
  DST-aware). Timed events emit `;TZID=<zone>` + a matching `VTIMEZONE`, preserving Palm
  wall-clock literally (exact round-trip). Foreign `...Z` UTC inputs convert to device
  local with correct DST offset. Floating (no zone) when `BRIDGE_TZ` is unset.
- **VALARM** — Palm alarm advance/unit ↔ `TRIGGER:-PT<n>M/H` / `-P<n>D`.
- **EXDATE** — Palm exception dates ↔ `EXDATE` (value-type-matched to DTSTART).
- **Charset** — Palm CP1252/Latin-1 ↔ UTF-8 on all text fields (`charset.c`); curly
  quotes, accents, €, … round-trip byte-exact; unmappable UTF-8 → `?`.

### Still ahead
- **sync-token (RFC 6578) optimisation** — engine currently diffs a full PROPFIND
  Depth:1 ETag listing each run (correct + universal); a `sync-collection` REPORT would
  cut it to a delta. Slots in behind the existing ETag comparison.
- **Categories** — Palm AppInfo categories → DAV collections (nice-to-have).
- **Port to ESP32** — swap curl for esp_http_client+mbedTLS; PDB files on SD; cap the
  static reconciliation arenas (host proof uses MAXR=256 × 4 KB).

---
## Original feasibility spike (superseded by the above, kept for context)

## Verdicts reached
- **Emulator paths are dead on this board.** Dragonfruit needs 8 MB PSRAM; porting
  PumpkinOS whole = desktop-class deps (SDL2, dynamic linking, MMU, malloc-heavy) +
  a 150 KB full framebuffer. Neither fits no-PSRAM.
- **Native app + donated data layer = viable, no RAM wall** except the WiFi+TLS sync
  burst (~80–120 KB transient; manage via no full framebuffer + tuned mbedTLS +
  stream one record at a time).
- **PumpkinOS is the best donor:** `DateBook/DateDB.c` (ApptPack/ApptUnpack) and
  `AddressBook/AddressDB.c` (PrvAddrDBUnpack) are the original Palm SDK sources with
  the byte layout documented inline. **GPLv3** — lifting code = GPLv3 firmware;
  clean-room from the struct+format keeps you free.
- PumpkinOS **has** global Find (`libpumpkin/Find.c`, already a streaming/no-RAM
  design). PumpkinOS **stubs** Graffiti (`GrfProcessStroke` = "not implemented") —
  bring your own recognizer ($Q recommended for MCU; all KB-scale, fits budget).

## What these spikes prove (both build with `cc -Wall -O2` and run)
- `pdb_prototype.c` — generates a real `DatebookDB.pdb`, streams it back with a
  faithful endianness-corrected **ApptUnpack** port, emits iCalendar. Handles timed,
  all-day (`VALUE=DATE`), and weekly recurrence (`RRULE;BYDAY=`). Peak RAM = one
  ~1 KB record buffer; DB never resident.
- `addr_prototype.c` — same for `AddressDB.pdb` → **vCard 3.0**. Handles the Palm
  quirk that **email is a phone slot whose label==emailLabel** (not a field), phone
  type mapping, and company-only cards.

Seam confirmed: own ~30-line PDB container reader + tiny unpack; **zero** PumpkinOS
DataMgr/storage/MemHandle (the 8.7k LOC you feared dragging in never came).

## Gotcha found
Original PumpkinOS `ApptUnpack` has a bug in the exceptions loop (`dt[i].month`
assigned twice, `.day` never set). Ported code needs review, not blind copy.

## Still-missing / the real remaining work (see memory for ranking)
1. **Two-way sync engine** — uniqueID↔ETag map, dirty/delete tracking, conflict
   policy, RFC 6578 sync-token. Bigger than all the parsing combined. THE project.
2. **Download direction** — ICS/vCard *parser* + PDB *writer* (Pack + rebuild index).
   Spike only does upload.
3. **Timezones + charset** — Palm = local wall-clock + Palm Latin charset; DAV = UTF-8
   + VTIMEZONE. Small code, big data-loss risk if skipped.
4. **Record attributes** — category/dirty/delete/uniqueID live in the record INDEX
   entry attr byte + AppInfo block; needed for incremental (not full-replace) sync.
5. Nice-to-have: ToDoDB→VTODO, MemoDB→VJOURNAL/files, alarm→VALARM.

Board on the bench: CH340 @ /dev/ttyUSB0, ESP32-D0WD-V3 rev3.1, 4 MB flash, no PSRAM.
