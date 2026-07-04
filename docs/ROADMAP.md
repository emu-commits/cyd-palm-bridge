# CYD Palm ↔ iCloud bridge — roadmap & resume guide

> **STATUS (2026-07-04): both phases in this doc are DONE.** Phase A (contact/
> CardDAV sync) and Phase B (the ESP32 firmware port) are complete and on
> hardware. The active work is now the **PalmOS-style UI** — see
> **docs/UI_ROADMAP.md** and the per-step log **docs/BUILD_PROGRESS.md**. This
> file is kept as the historical sync/port plan and reference.

Written 2026-07-03 so this can be picked up cold. Two things remained then: **(A)
contact (CardDAV) sync**, then **(B) the ESP32 firmware port** — both since done.
Everything else was already validated against real iCloud.

---
## 0. Resume in 30 seconds

```
cd ~/cyd-palm-bridge
make                      # builds roundtrip, bridge_cli, incremental, synctoken, category
make test                 # codec gate (no server) — 285 checks
# Radicale (dev server) for the server-backed gates:
./davvenv/bin/python -m radicale --config radicale.conf &   # localhost:5232, creds palm:palm
make itest stest ctest    # incremental / sync-token / category gates
./tests/dav_roundtrip.sh  # PDB->server->PDB
```

Live iCloud (works today): set `DAV_BASE=https://caldav.icloud.com`, `DAV_USER=<appleid
email>`, `DAV_PASS=<app-specific password with dashes>`, then:
```
./bridge_cli discover
./bridge_cli demo cal pdb/demo_cal.pdb && ./bridge_cli synccat cal pdb/demo_cal.pdb
./bridge_cli demo todo pdb/demo_todo.pdb && ./bridge_cli synccat todo pdb/demo_todo.pdb
```

## 1. Status (what already works)

- **Codecs both directions, byte-lossless** (`make test` = 285/0): DateBook↔VEVENT,
  Address↔vCard, ToDo↔VTODO, plus VALARM, EXDATE, timezones (`tz.c`, DST-aware,
  VTIMEZONE), CP1252↔UTF-8 (`charset.c`), and the AppInfo category table (`appinfo.c`).
- **Incremental conflict-aware sync** (`sync.c :: sync_collection`): hash+ETag change
  detection, conditional PUT/DELETE, server/local/keep-both policies. `make itest`.
- **RFC 6578 sync-token delta** (`dav.c :: dav_sync_report`, `sync.c :: buildServer`):
  delta when idle, full-resync + PROPFIND fallbacks. `make stest`.
- **Category → collection routing** (`sync.c :: sync_categorized`, CLI `synccat`):
  partitions by Palm category, name-matches labels to same-named collections. `make ctest`.
- **iCloud LIVE, validated**: discover lists Work/Home/Reminders; `synccat cal` pushed
  4 events routed to Work/Home and pulled 2 pre-existing; `synccat todo` pushed 2 VTODOs
  to the Reminders list and pulled 1. Auth = app-specific password. XML parser is
  namespace-tolerant (iCloud stamps `xmlns="DAV:"` on every element — see `xml_open`/
  `xml_text` in `dav.c`).
- Transport today = shells to the `curl` binary (`dav.c`). Records held in RAM in static
  arenas (`MAXR=256 × PALM_REC_MAX=4096`) — fine on host, **must shrink for the MCU**.

Pending live confirmations (not blockers): 2nd-run idempotence vs iCloud and a
phone-edit→pull. Expected to pass.

---
## PHASE A — Contact (CardDAV) sync   ← do first

**Why it's not already working:** iCloud CardDAV lives on a **different host** than
CalDAV. `caldav.icloud.com` → `pNN-caldav.icloud.com` serves calendars only; contacts
are on `contacts.icloud.com` → `pNN-contacts.icloud.com`. Our `resolveColls(&d,'a',…)`
ran `addressbook-home-set` against the caldav host and got nothing back.

Design decisions already locked:
- iCloud exposes **one** address book (single CardDAV collection). So contacts use
  `sync_collection` with `KIND_CARD` — **not** `sync_categorized`. Categories are
  preserved on the record (attr nibble) but **not routed** (no per-category collections
  on iCloud; groups are out of scope).
- vCard already emits `UID` + `VERSION:3.0` (iCloud requires both) and round-trips the
  email-as-phone-slot quirk; charset handled.

### A. Steps

1. **Separate CardDAV base.** Add env `DAV_CARD_BASE` (default `https://contacts.icloud.com`).
   In `main.c`, contact discovery/sync must start from that host, not `DAV_BASE`.
   - Simplest: a helper `cardBase(d)` that returns a `DavCtx` copy whose `base` is
     `DAV_CARD_BASE` if set, else if `DAV_BASE` contains `caldav.icloud.com` swap the
     `caldav` token → `contacts`, else fall back to `DAV_BASE`.
2. **Card discovery.** Reuse `resolveColls(&cardCtx,'a',&cs)` — it already does
   current-user-principal → `addressbook-home-set` → list collections filtered to kind
   `'a'`. It should now find the address book once pointed at the contacts host.
   `absPath()` already handles the absolute-URL hrefs iCloud returns.
   - Verify `dav_list_collections` classifies `<addressbook/>` resourcetype as `'a'`
     (it does via `strcasestr_range(...,"addressbook")`).
3. **Contact sync command.** Add `bridge_cli synccontacts <pdb>` (or extend `sync`):
   - resolve the card base, discover address books, pick the collection (first one, or
     `DAV_CARD` path if the user set it),
   - `sync_collection(&cardCtx, pdb, pdb, that_coll, KIND_CARD, "state/contacts.map", pol, &st)`.
   - Print the discovered collection + stats, same style as `synccat`.
4. **`demo card` generator.** Extend `cmd_demo` for `KIND_CARD`: write an `AddressDB.pdb`
   with a couple of vCards (name/company/phone/email) so there's something to push.
   Reuse the `Addr`/`AddrIntern` builder pattern from `tests/roundtrip.c :: mkAddr1`.
5. **Wire discover to show address books.** `cmd_discover` should call `resolveColls`
   on the **card base** for the addressbook section (right now it uses the same `d`, so
   iCloud returns empty). Make discover try `DAV_CARD_BASE`/contacts-host for the `'a'`
   pass.

### A. Acceptance (against iCloud)
- `./bridge_cli discover` now lists the address book under "addressbooks:".
- `./bridge_cli demo card pdb/demo_card.pdb && ./bridge_cli synccontacts pdb/demo_card.pdb`
  → `+N push`, and any pre-existing contacts show as `pull`.
- Second run → `0 push/pull` (idempotent).
- Edit a contact on the phone → next run reports `~1 pull` and updates the PDB.
- Check the Contacts app shows the demo contacts; delete them after.

### A. Risks / notes
- iCloud vCard normalization may reorder properties / add `X-` fields → an object may
  look "changed" on the first pull-back. If a benign re-pull loop appears, compare on a
  normalized vCard (sort properties) instead of raw ETag, or accept one settle cycle.
- Some iCloud accounts have multiple address books; default to the one whose href ends
  in `/card/` if present.

---
## PHASE B — ESP32 firmware port (base CYD, ESP32-2432S028R, **no PSRAM**)

Goal: run the exact same codec + sync engine on-device, headless first (no UI), syncing
`.pdb` files on the SD card to iCloud over Wi-Fi. Keep `dav.h`'s function contract so
`sync.c` and all codecs compile unchanged.

### B1. Transport: replace curl with on-device TLS HTTP  ← the big one
- `dav.c` currently builds shell strings and `popen("curl …")`. Write `dav_esp.c`
  implementing the **same `dav.h` API** (`dav_put/get/delete/getetag/list/sync_report/
  prop_href/effective_host/list_collections`) over **mbedTLS**.
- **Decision:** esp_http_client's method enum may not cover `PROPFIND`/`REPORT`/
  `MKCALENDAR`. Prefer a **hand-rolled minimal HTTP/1.1 client over an mbedTLS socket** so
  we control the method verb, headers (Depth, If-Match, Content-Type), Basic auth, and —
  crucially — can **stream** the response body straight into the XML/ICS/vCard parsers
  without buffering the whole thing. (Falls back to chunked reads; iCloud responses can
  be large.)
- TLS: use `esp_crt_bundle` (Mozilla CA bundle) for `*.icloud.com`. Basic auth header =
  `Authorization: Basic base64(user:pass)`.
- Keep the request bodies (PROPFIND/REPORT/sync XML) identical to what `dav.c` writes to
  `state/.dreq`/`.sreq` today.
- Reuse the namespace-tolerant `xml_open`/`xml_text` and the ICS/vCard parsers as-is.

### B2. Storage: PDBs + state on SD
- Mount SD (SPI) via `esp_vfs_fat_sdspi_mount` at `/sdcard`. Then `pdb.c` and `sync.c`
  file I/O (`fopen`/`fread`/`fseek`/`fwrite`) work nearly unchanged — just repoint paths
  (`pdb/…`, `state/…`, `state/.body`) under `/sdcard`.
- Confirm the CYD's SD pins/SPI bus don't conflict with the ILI9341 display SPI.

### B3. RAM: kill the big static arenas  ← the no-PSRAM constraint
- Today `sync.c` holds **every record in RAM**: `S.locArena` and `Out.arena` are
  `MAXR(256) × PALM_REC_MAX(4096)` = ~1 MB **each**, and `sync_categorized` uses three
  (`all`, `o`, `sub`). Impossible in ~300 KB.
- Plan:
  1. Drop `MAXR` (e.g. 96) and `PALM_REC_MAX` (e.g. 2048) for the device build — realistic
     PIM DBs are a few hundred small records; make these build-time configurable.
  2. **Stream the output PDB**: instead of accumulating all kept records in `Out.arena`,
     write records to the output `.pdb` on SD as they're decided (two-pass: pass 1 counts
     + computes offsets/index, pass 2 writes), so `Out.arena` disappears.
  3. Load local records **lazily** from SD per uid during reconciliation rather than one
     big `locArena`; the node table (uid/attr/hash/offset) stays tiny.
  4. For `sync_categorized`, process **one collection at a time** re-reading the PDB per
     category (SD is cheap) instead of holding `all` + `sub` simultaneously.
- Net: resident state becomes the node/map tables (KBs) + one record buffer + the TLS
  buffers. This is the real device engineering task; host behavior must stay identical
  (keep the host build using the big arenas, gate device sizes behind a macro).

### B4. Clock + Wi-Fi (needed before TLS)
- CYD has **no RTC**; TLS cert validity needs correct time. Do **SNTP on Wi-Fi connect**
  before any HTTPS (reuse the lesson from [[project_cardputer_wifi_caldav]] — SNTP-on-
  connect is the real fix). Persist epoch + software-RTC checkpoint across the sync.
- Wi-Fi provisioning: SSID/pass from NVS or a config file on SD.

### B5. mbedTLS tuning
- iCloud TLS handshake + records: tune `MBEDTLS_SSL_IN_CONTENT_LEN`/`OUT_CONTENT_LEN`
  down where possible; watch peak heap during handshake (the known ~40–70 KB spike).
- If heap is tight, run sync with Wi-Fi up but display/other subsystems quiesced.

### B6. Trigger / minimal UX
- Headless first: run one sync on boot (or on a button press / timer). No Graffiti or
  screens yet — that's a separate scope once the bridge is proven on-device.

### B. Build system
- ESP-IDF project. Put the codecs + sync as an **IDF component** compiled unchanged:
  `pdb.c datebook.c address.c todo.c ical.c vcard.c tz.c charset.c appinfo.c sync.c`
  (+ `dav_esp.c` instead of `dav.c`). Host tests keep using `dav.c`/`curl` + the Makefile.
- Guard device-only sizes/paths behind `#ifdef ESP_PLATFORM`.

### B. Acceptance
- On-device: read a real `.pdb` from SD, `synccat cal` to iCloud over Wi-Fi, see events
  on the phone, second run idempotent, memory stable (no leak across repeated syncs).

---
## Open questions to settle when we resume
- Contacts: confirm iCloud returns the address book via `addressbook-home-set` on
  `contacts.icloud.com` (Phase A step 2) — if not, fall back to the well-known
  `/.well-known/carddav` on that host.
- Device transport: hand-rolled mbedTLS HTTP client vs. bending `esp_http_client` to
  custom verbs — decide in B1 (leaning hand-rolled for streaming + method control).
- Whether to keep two-way sync on-device v1 or start **download-only** (server→PDB) to
  de-risk, then add upload.

## Key file map (host, today)
- `bridge/dav.c` — DAV over curl; XML helpers `xml_open/xml_text` (ns-tolerant, bounded).
- `bridge/sync.c` — `sync_one` (core), `sync_collection`, `sync_categorized`,
  `buildServer` (sync-token), `MAXR`/arenas (B3 target).
- `bridge/main.c` — CLI: discover/push/pull/sync/synccat/demo/dump; `resolveColls`,
  `absPath`, `cmd_demo`, `cmd_synccat`, `diagnose`.
- `bridge/{datebook,address,todo,ical,vcard,tz,charset,appinfo,pdb}.c` — codecs/container.
- `tests/` — roundtrip.c, incremental.c, synctoken.c, category.c, dav_roundtrip.sh.
