# Next steps plan (2026-07-09)

Snapshot after the sync-correctness session. Newest state on top; see
`docs/BUILD_PROGRESS.md` for the running build log and the deep-dive on each fix.

## UPDATE 2026-07-10 — UI polish sprint (sync/Date Book parked as "ok for now")

Three-part pass on the UI, docs updated between each:
1. **Timezone picker + DST — DONE.** Free-text `PF_TZ` replaced with a picker over
   `clock.c`'s DST-aware zone table; guarantees a mapped zone so DST fires. Header
   shows live offset/DST. (`clock_zone_*` + `clock_now_desc` + `show_tz_picker`.)
2. **`lv_table` list backbone — DONE.** `list_view` rewritten as one virtualized
   `lv_table` (compact cell text + visible-only draw); the `LIST_MAX=12` cap is
   gone. Row→uid via a malloc'd parallel array resolved in the click handler.
3. **Palm-lens Address/ToDo/Memo — DONE.** Address gets a Graffiti **Look Up**
   name-prefix filter (composes with the category trigger); To Do gets a checkbox
   column (tap col 0 = toggle done) + a **Show/Hide Completed** toggle in Options;
   Memo keeps the authentic first-line list. All inherit the un-capped table.
   *Deferred:* record sorting (Address by last name, To Do by priority/due) — needs
   row buffering; revisit after on-device performance check.

**VERIFIED ON DEVICE + COMMITTED (`7dead10`, pushed to origin/main).** TZ picker
selects + shows live DST, Address Look Up filters by first/last name, record
tap-to-detail and To Do checkbox toggle work, >12-record lists browse (cap gone).
Two device-found bugs fixed in the same commit: the TZ picker froze as a 24-button
`lv_list` (rebuilt on `lv_table`); and `lv_table` tap read nothing because LVGL
9.2 clears the selected cell on RELEASED before `LV_EVENT_CLICKED` — now read on
`LV_EVENT_VALUE_CHANGED`.

### What's next (after the UI-polish sprint)

### DONE 2026-07-10 (part 2) — sorting, Find, top-bar clock, sync progress (flashed)

- **Record sorting — DONE.** `build_record_table` collects rows into a malloc'd
  `SRow[]`, `qsort`s (Address/Memo by name via `strcasecmp`; To Do incomplete →
  priority → text), fills the table in order. Transient buffer, interactive mode.
- **Find UI (P1.7) — DONE.** Silkscreen Find opens `show_find()`: Graffiti query
  field + results `lv_table` over `find_in_pdb` across all four PDBs; tap a hit
  opens the record in its app. Added `data_db_path()`.
- **Top-bar clock — DONE.** 12h `clock_lbl` centered in the title bar, 15 s
  `lv_timer`; persists across screens.
- **Sync progress — DONE (coarse, TEXT).** `hotsync_progress()` (0..100 per
  collection) is shown as a `<p>%` line in the status label. NOT an `lv_bar`: a bar
  froze the screen at 66% because it forces an LVGL draw-layer alloc that can't be
  satisfied mid-sync (fragmented heap) → LVGL live-locks → Task WDT. A finer
  *intra*-collection bar still needs a `sync_collection` callback, and any future
  graphical progress must avoid layer-compositing widgets during the sync window.

All four build clean + flashed to /dev/ttyUSB0 (boots clean, ~193 KB free heap).

## UPDATE 2026-07-10 (part 4) — sync idempotency on iCloud relocation (DONE, host-green)

The parked "idempotency on live iCloud href relocation" item is fixed at the
engine level. Root cause: when iCloud relocated an object and its GET-for-UID
truncated on the 8 KB no-PSRAM fetch buffer, `resolveServer` fell back to
`uidHash(href)`, minting a divergent identity → spurious delete + phantom pull =
duplicate + local loss. Now an unresolvable object is **deferred** (never gets an
href identity) and **all deletes are suppressed** that round (delete-candidate
rows staged to `SV_MO`, `present` decided post-enumeration; any `unresolved`
forces present+unchanged). New gate `tests/idempotent.c` (`-DOBJ_FETCH_CAP=4096`)
reproduces the truncation on the host and proves: etag churn converges, and an
unresolvable relocation causes no delete/dup/loss and converges when resolvable.
Added relocation telemetry for the on-device iCloud trace. **On-device verify
against real iCloud still pending** (needs a photo-heavy contact to exercise the
truncation path live).

### What's next

- **To Do "due date" sort + Details — DONE (2026-07-10 part 5).** Due date is
  threaded through the row (`data.c` secondary `"pri %d due %d"`), rendered
  Palm-style with a right-aligned M/D, and an Options "Sort by Due Date" /
  "Sort by Priority" toggle (`g_todo_sort_due` + `cmp_todo_due`) picks the order.
  Detail already showed `Due: M/D/Y`. *Follow-up:* editing a due date needs a
  date picker in the To Do edit form.
- **Finer sync progress — DONE (2026-07-10 part 6).** Engine progress hook
  (`sync_set_progress`) ticks per reconciled record; `hotsync` maps it into each
  collection's band, so the `%` climbs within a collection. Stays text (no
  `lv_bar`, per the layer-alloc WDT rule).
- **P2 hardware:** U8 power / battery gauge (GPIO34) + light-sleep, U9 case.
- **Backlog (`UI_ROADMAP.md`):** battery % in the title bar (next to the new
  clock), RSS reader, Preferences app icon, power/reset-button remap, dark mode.

The real open sync item (idempotency on live iCloud href relocation) stays parked
under "sync + Date Book are ok for now."

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

5. **On-device configuration (`config.ini`).** *All three chunks DONE and on
   `main` (alongside the streaming sync); compile-verified (`idf.py build`), NOT
   yet flashed.*
   - **Chunk 1 DONE (`c536a19`):** the device loads `/sdcard/config.ini` at
     runtime via `firmware/main/appcfg.[ch]` (precedence: `config_defaults()` <
     `secrets.h` seed < `config.ini`). HotSync reads Wi-Fi / iCloud / per-app
     collections / policy from `appcfg()` instead of the compile-time macros. So
     creds + collections are editable by hand-editing the SD card — no reflash.
     `DavCtx.user` grown 64→128.
   - **Chunk 2 DONE:** Preferences editor UI (`ui.c` `show_prefs`) — reached from
     the **Options → Preferences** menu. A scrollable Graffiti form over all the
     config fields (Wi-Fi, Apple ID + app password, CalDAV/CardDAV hosts, the
     three collections, time zone) plus a cycling **Conflicts** policy button;
     **Save** writes `config.ini` via `appcfg_save()` (toast on success/fail).
   - **Chunk 3 DONE:** collection-discovery screen (`ui.c` `show_discover` +
     `hotsync.c` `discover_task`) — the Preferences **"Discover collections…"**
     button brings Wi-Fi up, walks the iCloud CalDAV **and** CardDAV homes
     (`current-user-principal` → `calendar-home-set` / `addressbook-home-set` →
     `dav_list_collections`), and lists every calendar / reminders list / address
     book. Tap one → assign it to a role (Calendar / Reminders / Address); the
     href is normalised to the no-slash form sync expects and written into the
     in-memory config. **Back** returns to Preferences, where **Save** persists.
   - **Not yet done — on device:** flash `main` and verify the form edits + a live
     discovery round-trip against a real iCloud account. (The Apple ID `@` is now
     typable on-device — item 6 done.)
   This is the single biggest gap between "our prototype" and "a PDA someone can
   set up."

6. **Graffiti punctuation. — DONE.** PalmOS-style punctuation shift: a single tap
   arms punctuation mode (a "PUNC" indicator lights in the strip), and the next
   stroke is matched against a dedicated punctuation set (`PTMPL` in
   `graffiti.c`); period is the tap that follows the shift (two taps), exactly
   like real Graffiti. Starter strokes for `@ , / - ' ( ) ?` (plus `.`); `_` is
   shape-identical to `-` under the size-normalizing $1 matcher so it's omitted.
   Like the letters, the strokes are a coarse starter — tune on-device from the
   `graf pnc` telemetry. *(Compile-verified; on-device stroke tuning pending.)*

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

P0.2 (UID matching) + P0.3 (streaming/large collections) — DONE, merged to `main`
(PR #2), awaiting on-device verify of a large collection.
P1.5 (on-device config: runtime load + Preferences UI + discovery) — DONE, merged
to `main`, awaiting flash + on-device verify.
Next: flash `main` and verify config on device →
P1.6/7 (Graffiti punctuation, Find UI) → P2 (hardware).
