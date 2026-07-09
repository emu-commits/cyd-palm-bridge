# Next steps plan (2026-07-09)

Snapshot after the sync-correctness session. Newest state on top; see
`docs/BUILD_PROGRESS.md` for the running build log and the deep-dive on each fix.

## UPDATE — sync closed out (branch `claude/sync-uid-streaming`)

Two structural gaps below are now DONE (host-green; on-device verification of
Stage 2 pending):

- **P0.2 UID-based identity — DONE.** Reconciliation keys on the object's
  iCal/vCard UID hash (bridged through the map), not the href name. Kills the
  relocation / lost-map dup-UID class. Foreign objects push back with their own
  immutable UID. New gate `tests/uidmatch.c`. Commit `e9eb288`. *(Verified on
  hardware.)*
- **P0.3 large collections — DONE.** Reconcile is a disk-backed 3-way merge-join
  (streaming); peak RAM during a DAV op is O(1) in record count, so the MAXR=24
  hard cap is gone. `tests/bigsync.c` runs 200 records under device sizing.
  Commit `e9d12cd`. *(Awaiting on-device confirmation with a large collection.)*
- Host harness `tests/gate.sh` added (reliable Radicale) — resolves the
  "Radicale doesn't work in the sandbox" blocker (old item 13).

Remaining P0: **idempotency audit on real iCloud** (etag round-trip) and the
on-device confirmations above. Then P1 below.

## Where we are

On-device iCloud sync is now **bidirectional and durable** for all three
collections (Date Book / To Do / Address). This session closed a chain of bugs
that had made sync silently lossy:

- **Pull worked for the first time** — `MAXR` was reverted 96→24 so the `S`
  struct coexists with the mbedTLS handshake (the no-PSRAM RAM fight). `ec878b8`
- **Local edits stopped being erased** — the sync map was frozen on the SD card
  because FATFS `rename()` fails over an existing file; now `remove()`+`rename`.
  Maps self-heal to the correct format with real etags. `8bd53da`
- **Failed pushes no longer counted or poison the map** — `pushLocal` keeps/maps
  only on 2xx; transient failures keep-local + retry. `afc3f25`
- **412 duplicate-UID conflicts resolved** — orphaned/duplicate records are
  dropped (server copy wins) or pulled, instead of looping forever. `254c115`
- **Calculator app** (`lv_buttonmatrix`) and **per-record Delete** (with confirm
  dialog) shipped. `5e6583c`, `9ce429f`

All commits are on `origin/main` (`254c115`).

## P0 — Finish the sync story (correctness / robustness)

1. **Confirm the 412 de-dup on-device.** Sync twice: expect
   `uid=… dropped: dup UID already on server`, `out` for Date Book to drop to 5,
   then a fully clean/idempotent second sync (`push=0 pull=0`, no `push FAILED`).
   Then verify no duplicate events remain in iCloud Calendar.

2. **Identity matching by UID, not href-derived uid (root-cause fix).** Today
   local↔server records are matched by a `uid` derived from the *href name*
   (`nameToUid`). iCloud-created objects have UUID hrefs that never align with
   Palm uniqueIDs, which is the source of the whole orphan/duplicate-UID class.
   Proper fix: key reconciliation on the iCal/vCard **UID** (stable across both
   systems) and always push to the mapped server href. Requires either fetching
   server object UIDs (a GET per object — fine for small collections) or keeping
   a UID↔href index. This eliminates the dup-UID problem instead of just
   resolving its symptom, and is the prerequisite for trusting sync at scale.

3. **Large collections (> 24 records).** `MAXR=24` caps each collection; real
   address books have hundreds. The record *bytes* already stream to disk, but
   `S` still holds `loc/map/srv[MAXR]` in one resident block that must fit beside
   the TLS handshake. Fix: split those arrays out of `S` into separate
   allocations AND page the server enumeration (process the REPORT in chunks)
   so the reconcile set isn't bounded by a single contiguous block. Biggest
   scaling blocker.

4. **Idempotency audit.** Confirm a pushed record converges to `LCLEAN`/`SCLEAN`
   on the next sync (etag stored from the PUT matches the server's REPORT etag)
   rather than re-pulling every time. Watch for iCloud rewriting objects
   server-side.

## P1 — Product completeness (usable by someone other than us)

5. **On-device configuration (`config.ini`).** *In progress on branch
   `claude/config-ini`.*
   - **Chunk 1 DONE (`c536a19`, compile-verified, not yet flashed):** the device
     loads `/sdcard/config.ini` at runtime via `firmware/main/appcfg.[ch]`
     (precedence: `config_defaults()` < `secrets.h` seed < `config.ini`). HotSync
     reads Wi-Fi / iCloud / per-app collections / policy from `appcfg()` instead
     of the compile-time macros. So creds + collections are now editable by
     hand-editing the SD card — no reflash. `DavCtx.user` grown 64→128.
   - **Chunk 2 (TODO):** Preferences editor UI — an LVGL form to edit the fields
     on-device (Graffiti entry) + `appcfg_save()` to write `config.ini`.
   - **Chunk 3 (TODO):** collection-discovery screen — Wi-Fi up → PROPFIND →
     pick your calendar / reminders / address book instead of pasting UUIDs.
   Chunks 2–3 are held until the sync PR merges, so they can be built + flashed +
   tested on a clean `main` (flashing now would clobber the sync test firmware).
   This is the single biggest gap between "our prototype" and "a PDA someone can
   set up."

6. **Graffiti punctuation.** Only `.` exists — no `@`, `,`, `/`, etc., so you
   can't type an email address (half-broken contact entry). Add a
   punctuation-shift mode like real Graffiti.

7. **Find UI.** Engine (`bridge/find.c`) is done; it needs a Graffiti query
   field and a results list.

8. **Finer sync progress.** Coarse `[k/M]` per-collection is done; a per-record
   bar needs a progress callback threaded through `sync_collection`.

## P2 — Hardware / polish

9.  **U8 power** — battery gauge (GPIO34), light-sleep for real battery life.
10. **U9 case** — enclosure.
11. **Graffiti training game** (backlog) and the **X** 2-stroke exception.

## Cleanup / housekeeping

12. **Gate the debug telemetry.** The `[sync]`/`[dav]` `fprintf(stderr)` lines
    are useful but chatty; put them behind a `SYNC_DEBUG` flag.
13. **Host test environment.** Radicale isn't functioning in the current sandbox,
    so the server-dependent gates (`run_gates.sh`, `bigsync`, `multiapp`,
    `category`) can't validate sync logic offline. Fix or document; the offline
    unit tests (calc/find/config/fuzz) do pass.
14. **Data hygiene in iCloud.** Clean up seed contacts (~12) and any duplicate
    events created during the broken-sync era before trusting the address book.

## Suggested order

P0.1 (confirm de-dup) → P1.5 (on-device config, unblocks real use) →
P0.2 (UID matching, unblocks trustworthy sync) → P0.3 (large collections) →
P1.6/7 (Graffiti punctuation, Find UI) → P2 (hardware).
