# Build progress log (durable — survives context loss)

Running log of the UI build (docs/UI_ROADMAP.md). Updated after each step so work
can resume cold. Newest phase on top.

> **Forward-looking plan lives in `docs/NEXT_STEPS.md`** (prioritized P0/P1/P2).
> This file is the historical log; that file is what to do next.

## SESSION 2026-07-10 (part 11) — iCloud Reminders dead end + drift self-heal

**The To Do mystery, solved.** A throwaway diagnostic build (`SYNC_DIAG_PROPFIND`,
shadow-PROPFIND per collection, since removed) proved the part-10 "config mismatch"
call WRONG. For the reminders list `ad54474b`: incremental `sync-collection`
returned 0 rows while a plain PROPFIND saw 3 members — and the user's 16 real
reminders were in NONE of them. Root cause: **Apple dropped CalDAV from Reminders
in iOS 13**; reminders created in the modern app / iCloud.com live in a proprietary
store no CalDAV client can reach. Discovery correctly finds the one CalDAV-visible
list ("Reminders ⚠️", `rn=6443`, not truncated); its 3 objects are the device's own
past test pushes, invisible to the native Reminders app. Not a bug — an Apple wall.

**Decision:** To Do stays on the iCloud CalDAV VTODO lane (works cross-CalDAV +
non-iCloud providers; invisible to native Reminders — acceptable, still cloud-backed).

**Code shipped:**
- **Streaming discovery** (`dav_parse_collections_stream` in `dav_xml.c`;
  `dav_list_collections` spools to `ENUM_SPOOL`): removes the residual 8 KB cap —
  the last non-streaming enumeration. `DAV_LIST_CAP` retired.
- **Window-boundary parser fix:** `dav_parse_members_stream` + the new collections
  parser dropped a record when `<response>` straddled the 4 KB window (discarded a
  partial tag); now keep the tail like `dav_parse_report_stream`.
- **`tests/streamparse.c` de-vacuumed:** used the `<d:response>` prefix but the
  parser matches unprefixed `<response>` (as real iCloud/Radicale send), so record
  comparisons had been 0==0. Fixed → immediately caught the boundary bug. Added
  home-set/collections coverage incl. reminders-after-calendars truncation.
- **Device always-full reconcile** (`ESP_PLATFORM`-gated in `sync_one`): clears the
  read token + never persists `#synctoken`, so every device sync full-enumerates.
  Fixes the drift where a failed first pull orphans a record (token advances past
  it → never retried; the 2-vs-3 To Do gap). Host keeps incremental + its gates.
- Discovery telemetry: `discovered [c] <name> (<href>)`.

Host: `make test` + `tests/gate.sh` GREEN. Firmware built (57% free) + flashed.
Unverified on device: To Do full-reconcile healing (out 2→3) + idempotent re-sync.

## SESSION 2026-07-10 (part 10) — streaming collection enumeration (8 KB cap gone)

Bulk-loaded iCloud + re-synced: Address pulled, but **Date Book + To Do didn't**.
Trace (`scratchpad/sync.log`) showed it was NOT a keep-alive regression (all
REPORTs `st=207`) — two separate things:
- **Date Book (64 events): the 8 KB enumeration cap.** The full sync-collection
  REPORT (~42 KB) truncated at 8191 bytes, the PROPFIND fallback truncated too, so
  `enumServer` returned failure and `sync_one` deliberately kept all local records
  and pulled nothing. The streaming reconcile removed the *reconcile* cap, but the
  initial server enumeration still buffered the whole etag list in one RAM buffer.
- **To Do: a config mismatch, not a bug.** Clean incremental delta, zero changes
  (`tok=incr rc=0 pull=0`, local recs=2) — the reminders were added to a different
  Reminders list than the mapped `…/calendars/ad54474b-…`.

**Fix (`722c6e0`): stream the enumeration.** `dav_sync_report` + `dav_list` spool
the reply to SD (`ENUM_SPOOL`, no size cap — `davreq` gains a spool mode via
`RespAcc.spool`, body streamed with `fwrite` in `on_event`, per-attempt truncate
on the keep-alive retry) and parse it with new sliding-window parsers
`dav_parse_report_stream` / `dav_parse_members_stream` (`dav_xml.c`): a 4 KB window
slides over the file emitting each `<response>` block and reads the trailing
sync-token from the final tail. Enumeration RAM is now O(1) in record count. The
8 KB RAM buffer remains only for discovery (`dav_list_collections`). New offline
gate `tests/streamparse.c` proves the streamed parse is byte-identical to the
buffer parse across window boundaries (wide entries, 300 recs, empty, deletes,
no-token, token-expired).

**VERIFIED ON DEVICE:** `REPORT … rn=42153 rc=0 (stream)` — Date Book reconciled
all 64 records and pulled the 2 new server events (`down +2`), heap stayed ~42 KB
free. Host `make test` + gate.sh green. To Do still needs its collection re-mapped
(Preferences → Discover) since its reminders live on another list.

## SESSION 2026-07-10 (part 9) — sync speed: TLS keep-alive connection reuse

User: "adding records on device and syncing seems fine, though very slow. Does it
need to be this slow?" — no. Root cause: `dav_esp.c` did
`esp_http_client_init/perform/cleanup` **per request**, so every DAV call opened a
fresh TLS connection and did a full handshake (~1-3 s against iCloud's cert chain
on the 160 MHz ESP32, software crypto). A first bulk pull of N records is **~2N
handshakes** — `resolveServer` GETs each new object for its UID (`sync.c:465`) and
`keepFromServer` GETs it again for its body (`sync.c:557`), plus each PUT/DELETE —
so sync time scaled with record count almost entirely in TLS setup.

**Fix (`20b33ed`): reuse one connection across requests.**
- `dav_esp.c` keeps a single `esp_http_client` handle alive (`keep_alive_enable`)
  bound to one origin (`scheme://host[:port]`); requests to the same origin reuse
  its TCP+TLS connection, so the handshake happens ~once per network phase instead
  of once per request. A different origin transparently rebuilds it.
- Headers that vary (Content-Type / Depth / If-Match) are deleted between reused
  requests so a prior PUT's If-Match can't leak into a later GET. The response
  accumulator moved to a static `s_acc` (the handle outlives each call's stack
  frame, so `user_data` can't carry it). One-shot reconnect retry handles a
  server-dropped idle keep-alive socket.
- **RAM invariant preserved:** new `dav_disconnect()` (dav.h; no-op on the host
  curl transport) is called from the engine's `sortFile()` — i.e. right before
  every heap-heavy in-memory sort — and from `wifi_down()`. So the ~40 KB TLS
  working set is never resident during a sort (the exact collision the per-call
  teardown originally avoided) and is always closed before the socket stack is
  torn down. Net: **~2N handshakes/collection → ~3**, peak heap unchanged.

Host gate suite green (sortFile's disconnect is a no-op on host); firmware builds
clean, boots ~193 KB free heap. Discovery also benefits (its PROPFINDs now share
one connection). **On-device before/after timing on the same record set is the
next check** — and the bulk-load-in-iCloud → pull-to-device test the user planned.

## SESSION 2026-07-10 (part 8) — on-device: light-sleep off, sync-awake, due picker, brightness slider

First on-device run of the part-3–7 sprint. Flashed `b302531`, then fixed what
hardware showed:

- **Automatic light-sleep disabled (`e821054`).** On this CYD the SoC light-sleep
  gates the APB clock between LVGL frames and the display+backlight glitch every
  cycle → the screen visibly **flashes on/off**. Commented out `CONFIG_PM_ENABLE`
  + `CONFIG_FREERTOS_USE_TICKLESS_IDLE` (the documented disable path) and rebuilt
  (`rm -f sdkconfig` so defaults re-apply). PWM backlight + idle screen-off are
  unaffected and still work. Light-sleep stays off unless a board tolerates APB
  gating during LVGL.
- **Screen no longer sleeps mid-sync (`d1d899a`).** The idle-blank timer kept
  running during a HotSync, so the display slept and — because the priority-4
  sync task starves the touch wake-poll — wouldn't relight on a tap until the
  sync finished. `idle_step()` now holds inactivity at zero (relighting if
  already blanked) whenever `hotsync_busy()`, keeping the progress line visible.
- **To Do due-date picker (`ef1a0d0`).** The edit form gained a **Due** trigger
  button opening a Palm-style popup: quick options (Today / Tomorrow / 1 Week /
  No Date) + an `lv_calendar` for an arbitrary day. Modal overlay over the form
  (same pattern as the category Details dialog) so typed Description/Note survive
  the pick; the chosen date writes `Todo.hasDue/dueY/dueM/dueD` on Save. Closes
  the part-5 "editing a due date needs a date picker" follow-up.
- **Preferences brightness slider (`ef1a0d0`).** A **Brightness: NN%** row opens
  a slider popup that live-drives `power_set_brightness()` as it's dragged and
  persists to `config.ini` on release (floor 10% so it can't blank fully).

All build clean (no warnings), flashed, boot clean at ~193 KB free heap. On-device
verification of the due picker + brightness UX and the sync-awake behaviour during
a real HotSync is the next hands-on check.

## SESSION 2026-07-10 (part 7) — U8 power: PWM backlight + light-sleep

New `firmware/main/power.c` / `power.h` wires the two `config.ini` fields that
existed but were dead (`brightness`, `backlight_sec`) and adds SoC light-sleep.
- **PWM backlight** on GPIO21 via LEDC, clocked from **RC_FAST/RTC8M** so the PWM
  keeps running through light-sleep (an APB-sourced LEDC would freeze the LED when
  the APB clock stops). `power_set_brightness()` sets the configured level;
  replaces display.c's plain always-on GPIO.
- **Idle screen-off** (the dominant battery lever on an always-on TFT): the LVGL
  run loop's `idle_step()` blanks the backlight after `backlight_sec` of LVGL
  inactivity; when off, it polls the raw touch panel (LVGL inactivity won't
  advance with the display idle) and wakes on the first press. The wake tap is
  **swallowed** (`g_swallow_tap` → `indev_cb` reports RELEASED until the finger
  lifts) so it can't fire a button — PalmOS wake behavior. Blanked, the loop idles
  in 120 ms slices for deeper sleep.
- **Automatic light-sleep** via `esp_pm_configure(light_sleep_enable)` (DFS
  `default→40 MHz`), enabled by `CONFIG_PM_ENABLE` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE`
  in `sdkconfig.defaults`. Wi-Fi/SD drivers hold PM locks while active, so a sync
  is never interrupted. **Disable path documented** in `sdkconfig.defaults` (comment
  the two lines; backlight + screen-off still work).

CMake: added `power.c` + `esp_driver_ledc`/`esp_pm`. Builds clean, 56% flash free.
**Not yet flashed / UNVERIFIED on hardware** — tickless idle + the bit-banged
touch/SPI display is exactly the kind of timing interaction that needs on-device
confirmation; watch for touch or display glitches and back the two sdkconfig
lines out if they appear. *Follow-up:* expose `brightness` as a live Preferences
slider (calls `power_set_brightness`).

## SESSION 2026-07-10 (part 6) — finer sync progress (per-record, intra-collection)

The status line's `%` now advances WITHIN a collection, not just as each of the
three starts. Added an optional progress hook to the sync engine
(`sync_set_progress(fn,ctx)` in `sync.h`): the engine calls `fn(done,total,ctx)`
once per reconciled record (`progTick` in the `sync_one` merge loop) with
`total` = the local record count (`progReset(nin)` in `sync_collection` /
`countRecs` in `sync_categorized`). Registered globally, so the ~15
`sync_collection` callers (tests) are untouched and remain no-ops. `hotsync.c`
registers `hs_prog_cb`, which maps `done/total` into the current collection's
band `[(step-1)/ntgt .. step/ntgt]`, so e.g. contact 40/120 of app 3/3 shows a
smoothly climbing number. **Stays text** — `hs_tick` still renders the number in
a label; no `lv_bar`/layer-compositing widget touches the fragmented heap during
the sync window (the WDT-freeze rule from part 2 holds). Server-only pulls can
push `done` past `total`, so the fraction is clamped to 100. Host gates + firmware
build clean. **Not yet flashed** (on-device testing deferred).

## SESSION 2026-07-10 (part 5) — To Do due-date sort + row display

The To Do list now threads the **due date** through the row and offers a sort
choice. `data.c` `cbTodo` extends the `secondary` string to `"pri %d due %d"`
(due = YYYYMMDD, 0 = none; priority stays leading so it's back-compatible). In
`ui.c`, `SRow` gains a `due` field; `collect_cb` parses it and renders the row
Palm-style — `"<pri> description        M/D"` with the due date right-aligned when
set. A new `cmp_todo_due` comparator sorts incomplete-first → earliest due
(undated sinks last) → priority → text, selected by a `g_todo_sort_due` toggle
added to the **Options** menu ("Sort by Due Date" / "Sort by Priority"). The
detail view already surfaced `Due: M/D/Y` (data.c `detTodo`), so no change there.
Firmware builds clean (57% free). **Not yet flashed** (on-device testing deferred).
*Follow-up:* the To Do edit form still can't SET a due date (needs a date picker);
sorting/among-existing-dates works today.

## SESSION 2026-07-10 (part 4) — sync idempotency: defer unresolvable relocations

**The bug (on-device, against real iCloud; host gates missed it).** iCloud
relocates an object to a UUID href and, when we then GET it to read its `UID:`
for identity matching, the no-PSRAM fetch buffer (8 KB) is too small for a
photo-heavy vCard, so the GET truncates and the UID can't be parsed. The old
`resolveServer` fell back to `uidHash(href)` — a *divergent* identity for what is
really an already-mapped record. Result: the mapped copy looked server-deleted
(spurious delete) **and** the object looked brand-new (phantom pull) = a
**duplicate + a lost local record**. Repeated syncs never converged. Radicale's
happy path never reproduced it because its GET always returns the full object.

**The fix (`bridge/sync.c`).** `resolveServer` no longer invents an href identity
when a UID can't be read. It **defers** the object (skips it this round) and
returns an `unresolved` flag; `sync_one` then **suppresses all deletes** for that
collection that round and drops the new sync-token so the deferred object is
re-reported next delta. To do this safely, delete-candidate rows (both map-only
absences and delta-reported deletes) are *staged* to `SV_MO` and their `present`
flag is decided only after the whole enumeration is known:
  - `'m'` map-only: unchanged if incremental, deleted if a full report;
  - `'d'` delta-delete: a real delete;
  - **any `unresolved` ⇒ every staged row forced present+unchanged** (no data loss).
A transient GET failure can now never delete or duplicate a record — worst case
it's a no-op that retries. Added **relocation telemetry**: `[sync] reloc uid=…:
map href=… -> server href=… (UID match)` and the `UID-resolve FAILED …` line, so
the next on-device iCloud sync is diagnosable over serial.

**New host gate `tests/idempotent.c`** (built with `-DOBJ_FETCH_CAP=4096` so a
bloated object overflows the fetch buffer on the host, reproducing the device
truncation deterministically). Two cases, both GREEN: (A) server-side etag churn
converges to a no-op with no re-pull loop / dup; (B) an unresolvable relocation is
deferred — no delete, no dup, no loss — and converges once the object is
resolvable again. Wired into `tests/gate.sh` + the `all`/`clean` Makefile targets.
Full gate suite + offline units GREEN; device firmware compiles clean.
**Not yet flashed** (device attached but on-device testing deferred to later).

## SESSION 2026-07-10 (part 3) — Preferences persistence bug

**Bug:** the timezone (and every other Preference) did not survive a reboot.
Root cause: the Preferences editor mutated `appcfg_mut()` in RAM only; the sole
write to `/sdcard/config.ini` was the explicit **Save** row. Picking a zone,
editing a field, toggling the conflict policy, or assigning a discovered
collection to a role never touched the SD card, so anything not followed by a
manual Save was lost at power-off.

**Fix:** call `appcfg_save()` at every edit-commit point so each change is
persisted immediately — `tz_tbl_click_cb` (timezone), `pf_edit_save_cb`
(single-field editor), `pf_pol_row_cb` (policy toggle), and `role_pick_cb`
(discovery role assignment). The manual **Save** row stays (harmless, and still
gives the toast). Flashed to /dev/ttyUSB0.

## SESSION 2026-07-10 (part 2) — sorting, Find UI, top-bar clock, sync progress

Four features on top of the UI-polish sprint; all build clean, flashed to
/dev/ttyUSB0 (boots clean, free heap ~193 KB, no panic).

**(1) Record sorting.** Lists were raw PDB order; now Palm-sorted. `build_record_table`
gained a collect→qsort→fill pass: `collect_cb` buffers each kept row (`SRow`:
uid + display text + case-folded sort key + To Do done/priority) into a malloc'd
array, `qsort` with `cmp_name` (Address + Memo, `strcasecmp` on the name — the
"Last, First" primary sorts by last name) or `cmp_todo` (incomplete first, then
priority 1..5, then text), then the table is filled in order and freed. Transient
buffer only, interactive mode (no TLS competing). Date Book day view already
sorted by time — untouched.

**(2) Find UI (P1.7 — engine `bridge/find.c` was already host-tested).** New
`show_find()` on the silkscreen **Find** button (was a stub): a `Find:` Graffiti
query field + a results `lv_table`. Each keystroke re-runs `find_in_pdb` over all
four PDBs (small files, interactive mode) into a capped (60) `FindHit` buffer;
rows show `"<app>: <snippet>"`, tap opens the record in its app (sets `cur_app`
from a FIND_*→AppDef map, then `show_detail`). Added `data_db_path(app)` so Find
can reach the raw PDB files. `free_finds()` + `g_findtbl` reset in `kill_kb`.
NOTE the FIND_* enum order differs from APP_* — mapped explicitly (`FIND_DBS`,
`find_appdef`).

**(3) Top-bar clock + date (Palm shows the time up top).** A `clock_lbl` centered
in the title bar, refreshed every 15 s by an `lv_timer` (`clock_tick`): 12h time
with the date to its right, `"12:34p  Jul 10"` (`CAL_MON[]` abbrev + day). Lives
on the title bar (not `content`) so it persists across screen swaps. Center stays
clear of the left title + right category trigger.

**(4) Sync progress — TEXT percentage (not an lv_bar).** `hotsync.c` exposes
`hotsync_progress()` (coarse 0..100, -1 idle) set at the per-collection step
(`setprog`); a finer intra-collection bar would still need a callback through
`sync_collection`. `hs_tick` appends `"\n<p>%"` to the status label while a sync
runs.
  - **WHY NOT AN lv_bar (device WDT, fixed):** the first cut used an `lv_bar`; it
    **froze the screen at 66%** (during the 3rd collection). addr2line on the Task
    WDT backtrace: `lv_timer_handler → lv_display_refr_timer → draw_buf_flush →
    lv_draw_dispatch → lv_draw_layer_alloc_buf → lv_draw_buf_create → lv_tlsf_malloc`
    with **CPU0 running `main`** (the LVGL task) starving IDLE0. Root cause: an
    `lv_bar` makes LVGL allocate a draw-**layer** buffer to composite its
    indicator; during a sync the heap is fragmented (Wi-Fi+TLS hold the big
    blocks) so that alloc can't be satisfied, and LVGL **spins retrying the draw
    every refresh** → WDT → frozen. A label needs no layer, so progress is text.
    **RULE: don't repaint layer-compositing widgets (lv_bar, opacity/transform,
    rounded-mask) during a sync on this no-PSRAM device — the layer alloc fails
    and LVGL live-locks. Use plain labels/rects for anything that redraws while
    Wi-Fi+TLS are up.**

## SESSION 2026-07-10 — UI polish pass (TZ picker → lv_table → app overhauls)

Three-part sprint after the user called sync + Date Book "ok for now". Working
through: (1) timezone picker + DST, (2) an `lv_table` list backbone, (3) applying
the Date Book "date-centric" lens to Address/ToDo/Memo. Docs updated after each.

**(1) Timezone picker + DST — DONE (builds clean).** The DST *logic* already
existed: `clock.c`'s IANA→POSIX map carries the transition rules (e.g.
`EST5EDT,M3.2.0,M11.1.0`), so `localtime()` applies DST automatically by the
current date once `tzset()` runs. The gap was the *input* — `PF_TZ` was a
free-text Graffiti field, so a typo fell through to `UTC0` (no DST). Fix:
- `clock.c` — the zone map moved to a file-scope table `TZ_TBL[]` (was a local in
  `iana_to_posix`); added `clock_zone_count()`/`clock_zone_name(i)` to enumerate
  it and `clock_now_desc(out,cap)` which formats the live wall clock as
  `"EDT -0400 (DST)"` via `strftime("%Z %z")` + `tm_isdst`. Added a few zones
  (UTC, Sao_Paulo, Dublin, Moscow, Dubai, Auckland).
- `ui.c` — new `show_tz_picker()`: a scrollable `lv_list` of the built-in zones
  with a header showing the current zone + live offset/DST. Tapping a zone stores
  the IANA name in `config.timezone`, calls `clock_set_tz()` immediately, and
  returns to Preferences. `pf_row_open_cb` routes `PF_TZ` here instead of the text
  editor. Guarantees a mapped zone → DST always fires.
- ⚠️ device-test watch: the picker is a 24-button `lv_list`; single-line buttons
  are the lightest kind, but if it exhausts the 24 KB LVGL pool on-device, switch
  it to the `lv_table` backbone from step 2.

**(2) `lv_table` list backbone — DONE (builds clean). Removes the LIST_MAX=12 cap.**
The generic `list_view` was an `lv_list` of N button objects. `lv_list` is NOT
virtualized: each row is a full button+label from the fixed 24 KB LVGL object pool
*and* needs a per-row draw task from that same pool — ~20-35 rows exhausted it
(StoreProhibited on the failed alloc, or a Task WDT when the draw timer spun). That
forced `LIST_MAX=12`, making records 13+ unreachable (the "… N more (not shown)"
footer never worked). Rewrote `list_view` around a single `lv_table`:
- One LVGL object regardless of row count; it stores compact per-cell text (not
  objects) and draws only visible rows. Row count is now bounded by free heap for
  the cell strings (KBs for hundreds of records), not by the object pool. **Cap
  gone.**
- Row→record identity: the table has no per-row user_data, so a parallel uid array
  `g_rowuids` (indexed by row) is malloc'd to the record count via a two-pass
  iterate (`tbl_count_cb` counts, `tbl_fill_cb` fills text + uid), resolved in
  `tbl_click_cb` via `lv_table_get_selected_cell`. Freed in `kill_kb`/on rebuild
  (`free_rowuids`).
- To Do rows keep the `[x]`/`[ ]` completion prefix from the data layer (step 3
  turns these into real columns). The Date Book Day view keeps its own bounded
  `lv_list` (one day of events) + the restored `row_cb`.
- `<stdlib.h>` added for malloc/free.

**(3) Palm-lens Address/ToDo/Memo — DONE (builds clean).** Applied the Date Book
"make a long list navigable" lens to the other three apps. `list_view` is now
app-aware, with a shared `row_keep()` filter applied in BOTH the count and fill
passes (so `g_rowuids` stays sized to what's shown), and `build_record_table()`
factored out so a filter change rebuilds only the table:
- **Address — Look Up (Palm's signature Address navigation).** A one-line
  `Look Up:` textarea sits above the list; Graffiti types into it (`active_ta`),
  and each keystroke (`LV_EVENT_VALUE_CHANGED`) mirrors to `g_lookup` and
  refilters by **case-insensitive name prefix** (`strncasecmp`, `<strings.h>`).
  The list rebuilds without tearing down the field (`g_listtbl` handle). The
  existing top-right category trigger still filters too — the two compose.
- **To Do — checkbox column + Show Completed.** Two-column table: col 0 =
  `[x]`/`[ ]`, col 1 = description (+ `pri N`). Tapping **col 0 toggles done**
  (`data_toggle_todo` + refresh); tapping the text opens detail. A **Show/Hide
  Completed** toggle in Options (`g_todo_show_done`, `act_toggle_done`) filters
  completed items via `row_keep`.
- **Memo — unchanged structure** (single-column first-line list is already the
  authentic Palm layout); it inherits the un-capped table + category filter.
- `g_listtbl` is nulled in `kill_kb` (it's a child of `content`, deleted by
  `lv_obj_clean` on every nav) to avoid a dangling delete.
- ⚠️ device-test watch: sort order is still PDB order (Palm sorts Address by last
  name, To Do by priority/due). Deferred — sorting needs buffering all rows, a RAM
  call better made after seeing the table perform on-device. Look Up + category +
  Show Completed already make the lists navigable.

**On-device fixes (2026-07-10, after first flash).**
- **TZ picker froze the device** → rebuilt on `lv_table` (was a 24-button
  `lv_list`; that many buttons exhausted the 24 KB LVGL pool → draw-task WDT
  freeze — the known failure class). Table row index == zone index, own click
  handler (`tz_tbl_click_cb`). Same pool-safe pattern as the record list.
- **Address Look Up only matched last name** → `row_keep` now also tests the
  first-name token after `", "` in the `"Last, First"` primary (case-insensitive),
  so Look Up matches by last OR first name like real Palm.
- Both flashed to /dev/ttyUSB0; first flash booted clean (free heap ~193 KB, clock
  restored from NVS, SD mounted, LVGL up, no crash).
- **lv_table tap selected nothing** (TZ pick did nothing; record-list tap was
  latently broken too). Root cause in LVGL 9.2 `lv_table` (`lv_table.c` ~594-606):
  on `LV_EVENT_RELEASED` the widget sends `LV_EVENT_VALUE_CHANGED` **and then
  resets** `row_act`/`col_act` to `CELL_NONE`; `LV_EVENT_CLICKED` is delivered
  *after* RELEASED, so a CLICKED handler always reads `CELL_NONE`. Fix: read the
  selection on **`LV_EVENT_VALUE_CHANGED`** (sent only on a genuine tap, not a
  scroll drag) for both the record list and the TZ picker. **RULE: for lv_table
  tap-to-select on this LVGL, use VALUE_CHANGED, never CLICKED.**

## SESSION 2026-07-09 — branch consolidation + Graffiti punctuation

**Branches consolidated onto `main`.** Assuming the streaming sync is good: PR #2
(`claude/sync-uid-streaming`) rebase-merged to `main`; `claude/config-ini` rebased
on top (only the two docs files conflicted — code was disjoint) and fast-forwarded
in; both feature branches + a stale plan branch deleted and pruned. `main` now
carries streaming sync **and** all three config chunks; config + streaming compile
together for the first time (`idf.py build`, clean).

**Graffiti punctuation (P1.6) — PalmOS punctuation-shift.** A single tap arms
punctuation mode (returns new `GRAF_PUNCT`; the strip lights a "PUNC" indicator),
and the next stroke is matched against a dedicated set (`PTMPL` in `graffiti.c`);
period is the tap that follows the shift (two taps) like real Graffiti. While
armed, the swipe/backspace gestures are bypassed so a horizontal stroke reads as
`-` not space. Refactored the $1 matcher into a shared `match_set()` (letters,
digits, punctuation all use it). Starter strokes for `@ , / - ' ( ) ?` (+ `.`);
`_` omitted (shape-identical to `-` under the size-normalizing matcher). The letter
set's old tap `.` template was removed (period is now the punct tap). Strokes are a
coarse starter — tune on-device from `graf pnc` telemetry, same as the letters.

## SESSION 2026-07-09 — on-device config.ini (chunks 2+3: Preferences UI + discovery)

Compile-verified (`idf.py build`, 58% flash free). Finishes the config story: the
device is now configurable end-to-end without a reflash or a host tool. Landed on
`main` alongside the streaming sync (PR #2 merged, `claude/config-ini` rebased +
merged) — one branch now carries both.

- **Preferences editor** (`ui.c` `show_prefs`, reached from **Options →
  Preferences**): a scrollable Graffiti form over the whole `Config` — Wi-Fi
  SSID/pass, Apple ID + app-specific password, CalDAV/CardDAV hosts, the three
  collection paths, and time zone — plus a cycling **Conflicts** button
  (iCloud/device/both). **Save** copies the fields into `appcfg_mut()` and writes
  `/sdcard/config.ini` via `appcfg_save()`, with a tap-to-dismiss alert reporting
  success or an SD-write failure. Reuses the record-editor `form_field` plumbing;
  `g_fields[]` grown 8→12 to hold the 10 text fields.
- **Collection discovery** (`ui.c` `show_discover` + `hotsync.c` `discover_task`,
  API in `hotsync.h`): the Preferences **"Discover collections…"** button (guarded
  against running mid-sync) brings Wi-Fi up on the shared background-task slot and
  walks *both* iCloud homes — `current-user-principal` → `calendar-home-set` and
  `addressbook-home-set` → `dav_list_collections` — collecting every calendar,
  reminders list, and address book into a small fixed array (`DiscColl[12]`). The
  `disc_prop` helper reuses `abspath` to follow iCloud's partition-host redirects
  (full-URL home-sets retarget `d.base`). Each found href is normalised to the
  no-leading/trailing-slash form sync stores. The UI polls `hotsync_status()`;
  when done it lists the results — tap one to assign it a role (Calendar /
  Reminders / Address, both offered for CalDAV kind since iCloud reports
  reminders as calendars). Assignments land in the in-memory config; **Back** →
  Preferences → **Save** persists them.
- Also: the menu's **Categories** item is now hidden when no data app is open (it
  was a no-op there).
- **Next:** flash + on-device verify (form round-trip + a live discovery against
  real iCloud).

## SESSION 2026-07-09 — on-device config.ini (chunk 1: runtime loading)

Branch `claude/config-ini` (`c536a19`), compile-verified, **not flashed** (flashing
would clobber the streaming firmware under test on the device). Delivers
hand-editable `/sdcard/config.ini`: Wi-Fi, iCloud login, per-app collections, and
conflict policy can change without a reflash.

- New `firmware/main/appcfg.[ch]`: the active runtime `Config`, loaded with
  precedence `config_defaults()` (safe iCloud hosts + timers) < a seed from
  `secrets.h` (a device with no config.ini behaves exactly as before) <
  `/sdcard/config.ini` (overrides any field it lists). The parser/serialiser
  (`bridge/config.c`) was already gate-tested; this is the firmware wiring.
- `hotsync.c` now reads all creds / collections / policy from `appcfg()` instead
  of the compile-time macros, and logs which source it used. Per-app PDB/map
  paths stay compile-time (device-local).
- `DavCtx.user` 64→128 (`bridge/dav.h`): an Apple ID email can exceed 63 chars;
  the old size would silently truncate → confusing auth failure.
- TODO chunks (queued until PR #2 merges, so they can be flashed + tested on a
  clean base): Preferences editor UI (LVGL form + Graffiti + `appcfg_save()`),
  collection-discovery screen (Wi-Fi → PROPFIND → pick collection).

## SESSION 2026-07-09 (cont.) — close out sync: UID identity + streaming

Branch `claude/sync-uid-streaming`. Two staged rewrites of `bridge/sync.c`, each
host-verified against Radicale via the new reliable harness `tests/gate.sh`.

**Stage 1 — reconcile by object UID, not href (`e9eb288`).** Identity was derived
from the href name (`nameToUid`); iCloud relocating an object to a new href, or a
lost map row, split one record into two → spurious deletes, uid churn, 412
duplicate-UID conflicts. Now reconciliation keys on a 64-bit hash of the object's
iCal/vCard UID, bridged through the map (new 5th column = the object UID). Foreign
objects re-push with their own immutable UID (a one-line UID rewrite in
`emit_object`). New gate `tests/uidmatch.c` (relocation + foreign-edit round-trip).
Map format v2 is back-compatible. **Verified on device** (sync "looks ok").

**Stage 2 — streaming merge-join reconcile (`e9d12cd`).** The array reconcile held
`loc/map/srv[MAXR]` resident during DAV calls, so it had to coexist with the
~35 KB mbedTLS handshake — the real reason a collection capped at 24. Now three
UID-hash index files are built on disk, sorted with no handshake live, then
merge-joined: only the current objhash's rows are resident during a DAV op, so
peak RAM is O(1) in record count and the cap is gone. `davreq` does
init/perform/cleanup per call, so mbedTLS is only live *inside* a call — the
sorts/joins between calls are handshake-free. `tests/bigsync.c` now runs 200
records under device sizing (was 24). Bugs squashed bringing it up: unsorted
`sv.raw` before the href join (dup record), untracked server-deletes polluting
`sv.idx`, a malformed index line truncating a whole source mid-merge (→ every row
SDEL), `freopen` (FATFS lacks it → `fclose`+`fopen`). **On-device confirmation of
a >24-record collection still pending.**

All host gates green (incremental, synctoken, category, uidmatch, bigsync@200,
multiapp) + offline (roundtrip/find/calc/config/fuzz). Firmware builds; sync RAM
is now much lower (no resident per-record arrays during TLS).

## SESSION 2026-07-09 — sync is finally correct end-to-end (on device)

Started from "multi-collection sync + Calculator flashed" and drove real on-device
testing. Each fix below was diagnosed from serial captures (`fprintf(stderr)` —
note the `dav` ESP_LOG tag does NOT reach the console here) and confirmed on the
CYD against live iCloud. **All commits on `origin/main` (through `638a838`).**

Chronological chain of bugs (each exposed the next):

1. **Calculator hung the UI** (`5e6583c`). 20 buttons in an FR-grid inside a
   flex-grow (indefinite-size) parent = LVGL auto-size layout recursion → Task WDT
   on the LVGL task (decoded via addr2line). Fixed with a single `lv_buttonmatrix`
   (also far lighter on the 24 KB LVGL pool). **Rule: avoid FR-grid + flex-grow
   parent + auto-sizing children on this build.**

2. **Per-record Delete** (`9ce429f`). Detail view now `Done | Delete | Edit`
   (To Do's Mark Done moved to its own row) + a PalmOS-style confirm alert; the
   menu Delete routes through the same dialog. (Dialog panel needs a FIXED height
   — `LV_SIZE_CONTENT` collapsed the prompt under the buttons.)

3. **Multi-collection sync enabled** — To Do → Reminders VTODO list, Address →
   `carddavhome/card` (CardDAV), discovered via PROPFIND, added to `secrets.h`.
   `[k/M]` progress in the status line.

4. **Pull had NEVER worked (every sync was push-only)** (`ec878b8`). The
   hardware-less `MAXR 24→96` bump made the single-`calloc` `S` struct too big to
   coexist with the mbedTLS handshake: at 96 the calloc failed (rc=-1); at 64 it
   starved TLS (`alloc(5140) failed`, `ssl_handshake -0x7F00`, ESP_ERR_HTTP_CONNECT
   on every request) so `buildServer` returned `nsrv=0` → reconciler thought the
   server was empty. **Reverted to the proven `MAXR=24`** (S ≈ 16 KB) + trimmed
   `DAV_LIST_CAP` 24→12 KB. Result: first real bidirectional sync —
   Date Book pull=5, To Do pull=2, Address pull=11, all `st=207`.

5. **Editing a record erased it on sync** (`8bd53da`). The per-collection sync
   map was FROZEN on the SD card: `sync_one` published it with
   `rename(tmp, mapfile)`, but **FATFS `rename()` fails if the target exists
   (FR_EXIST)** and the result was unchecked → the map stuck at its first-ever
   contents (an OLD 3-column `uid\thref\thash` format, no etag). The 4-column
   reader then read the hash into the etag slot → every record looked
   server-modified → every local edit became an LMOD+SMOD conflict → POL_SERVER
   discarded it. Fix: `remove(mapfile)` before `rename` (+ log failure). Self-heals
   to a correct 4-column map with real etags. **Rule: on this FATFS SD card,
   atomic-write-via-rename must `remove()` the target first.** (`pdbw_commit`
   writes the PDB directly, so the DB itself was never affected — only the map.)

6. **Failed pushes counted + poisoned the map** (`afc3f25`). `pushLocal` called
   `keepBytes` (PDB + map) and the callers did `pushNew++/pushMod++` regardless of
   status, so a failed PUT (a) inflated `+N up` and (b) wrote a bad-etag map row
   the next sync read as "server-deleted" → data loss. Now: fresh row + count only
   on 2xx; transient failures keep-local + preserve the OLD map row (retry).

7. **412 duplicate-UID loop resolved** (`254c115`). iCloud enforces one UID per
   collection (curl-verified: fresh UID → 201, same UID at a new href → 412).
   Records pulled from iOS/Mac (UUID href+UID) that lost their linkage got
   re-pushed to `<palmuid>.ics` → permanent 412. Fix: `pushLocal` returns 412
   without keeping; callers resolve per POL_SERVER — `LNEW`+412 drops the local
   orphan (server copy pulled under its own href), `LMOD`+412 pulls the server
   copy. Safe: a 412 proves the event is on the server.

**Verified on device:** bidirectional sync of all three collections; an edited
Address contact reconciles `LMOD+SCLEAN` (`sEtag==mEtag`) and pushes up (survives);
clean idempotent re-syncs (`Done: +0~0-0`). **Still to confirm next session:** the
412 de-dup converging Date Book to 5 records with no duplicates.

**Deeper root cause still open (see NEXT_STEPS P0):** local↔server matching uses a
uid derived from the href *name* (`nameToUid`), which doesn't align Palm uniqueIDs
with iCloud UUID hrefs — the source of the whole orphan/dup-UID class. Proper fix
is to match by the iCal/vCard UID.

## HARDWARE-LESS SESSION 2 (2026-07-05) — Find, Calc, Preferences, parser hardening

Four new host-tested modules in `bridge/` (shared by the firmware component and
the host tests — one source of truth). All RAM-conscious for the no-PSRAM board:
streaming/in-place where possible, bounded stack, no per-record heap churn.

- **Global Find (`bridge/find.c`, `find.h`).** Streaming case-insensitive
  substring search across DateBook/ToDo/Address (via codecs) + Memo (raw text),
  one record at a time, callback per hit with a snippet. Zero scratch buffers —
  memo bodies are matched in place in the reader's record buffer. `make` +
  `./find_test` (offline). Wired into the firmware build; the **Find UI is
  AWAITING** a Graffiti query field (deferred), so `find_cb` still just points at
  the ready `find_in_pdb()`.
- **Calculator (`bridge/calc.c`, `calc.h`).** Recursive-descent evaluator: + - * /,
  parens, unary +/-, decimals, precedence; returns CALC_OK / SYNTAX / DIVZERO.
  Recursion is depth-capped (64) so a pathological "((((…" can't overflow the
  device stack. `./calc_test`. Firmware `calc_cb` points at `calc_eval()`; the
  keypad UI is AWAITING FLASH.
- **Preferences backend (`bridge/config.c`, `config.h`, `firmware/config.ini.example`).**
  Parses/serialises a runtime `key=value` config on SD (Wi-Fi, iCloud creds,
  per-app collections, timezone, brightness, backlight timer, conflict policy) so
  Preferences can change them without a reflash. Line-at-a-time parse (one bounded
  stack buffer, no heap, no whole-file load); robust to hand-edits (unknown keys /
  malformed lines skipped, every copy length-bounded). `./config_test`. NOTE: holds
  passwords — treat as sensitive; never logged. **AWAITING FLASH:** having
  hotsync/app_main prefer `config.ini` over the compile-time secrets.h macros (kept
  that wiring out of a blind, untestable change to the working Wi-Fi/sync path).
- **Parser hardening (`tests/fuzz_test.c`, `make ftest`).** A malformed-input sweep
  built with **AddressSanitizer + UBSan**: truncate every valid VEVENT/VTODO/vCard
  and packed record at every byte, adversarial strings, crafted PDB headers, and
  120k random buffers through every parser (ical/vtodo/vcard, the unpackers,
  appinfo, and the PDB container). It **found a real heap-buffer-overflow**:
  `ical.c parseDT` read fixed offsets (v+4/+6/+9/+11) past a short/truncated
  DTSTART — i.e. an overflow on untrusted iCloud data. Fixed with length guards in
  `parseDT`; the identical pattern in `todo.c` DUE parsing fixed too. Also added a
  `PDB_MAX_RECS` cap so a corrupt header's record count can't trigger a ~0.6 MB
  index malloc on the device. Sweep is clean (no sanitizer report). Added to
  `run_gates.sh`.

All host gates green (`./tests/run_gates.sh`), clean `-Wall`.

## HARDWARE-LESS SESSION (2026-07-05) — engine streaming + multi-app HotSync

Work done with **no CYD attached** (laptop unavailable), so everything here is
host-provable; the firmware pieces compiled-by-inspection only and are flagged
**AWAITING FLASH**. All host gates green via the new one-command harness
`./tests/run_gates.sh` (spins up Radicale, runs every gate, tears it down).

1. **Streaming sync engine — the MAXR cap is lifted (gap #4 closed on host).**
   The reconciliation working set no longer holds record *bytes* in RAM:
   - `bridge/pdb.c` gained a **streaming writer** (`PdbW`/`pdbw_*`): kept records
     spill to a temp file (`state/.pdbout`); only a 24-byte/record index stays
     resident; `pdbw_commit` sorts by uniqueID and assembles the final PDB. This
     replaces the old in-RAM `Out.arena` (8 KB) + `Sink.map[MAXR]`.
   - `pdb_read_one(path,index,...)` added → local record bytes are read **lazily**
     from the source PDB during reconciliation instead of buffering them in
     `locArena` (the other 8 KB arena, now gone).
   - `Node` now stores **indices** into `map[]/srv[]` (was ~536 B of string copies
     per node → 16 B). This was the biggest per-MAXR RAM consumer.
   - Net: resident cost dropped from ~64 KB → ~17 KB at MAXR=24, so **device MAXR
     raised 24 → 96** (`bridge/sync.c`, `ESP_PLATFORM` branch) *within the old
     budget*, and there is no longer a record-byte arena wall.
   - Map file is now written atomically (`.tmp` + rename).
   - Proof: `tests/bigsync.c` (built `-DSYNC_DEVICE_SIZES` so the host uses the
     **device** MAXR=96) syncs **90 records** with ~300-byte notes (~27 KB, past
     both old walls): all push, idempotent, server-add pulls back. `make btest`.
   - **AWAITING FLASH:** on-device heap headroom at MAXR=96 + the extra SD reopen
     traffic from lazy reads (per-clean-record `pdb_read_one` reopens the file;
     an optional optimization is a persistent read handle).

2. **HotSync now syncs Date Book + To Do + Address, each to its own collection
   (gap #1 closed in code).** `firmware/main/hotsync.c` walks a target table and
   calls the proven `sync_collection` per app; Address uses the separate CardDAV
   host (`DAV_CARD_BASE` → contacts.icloud.com, resolved to `pNN-` at boot).
   Per-app collections live in `secrets.h` (see updated `secrets.h.example`):
   `SYNC_TODO_COLL` / `SYNC_CARD_COLL` (empty ⇒ skip that app). **Back-compat:**
   an old secrets.h with only `SYNC_COLL` still builds and syncs just the Date
   Book. **Memo is intentionally NOT synced** (iCloud Notes has no CalDAV/CardDAV
   surface). Proof of the engine path: `tests/multiapp.c` runs `sync_collection`
   for KIND_TODO (VTODO) and KIND_CARD (vCard) — push/idempotent/server-pull all
   green (`make mtest`). **AWAITING FLASH** for the ESP-IDF glue itself.

3. **Test hygiene (gap #6 fixed).** `tests/category.c` cleaned the wrong map glob
   (`state/cat_*.map`) but `sync_categorized` writes `state/palm_cat_*.map` — so
   stale maps leaked and made back-to-back runs flaky. Fixed to the real names;
   verified stable across repeated runs without wiping `state/`. New harness
   `tests/run_gates.sh` runs the whole suite from a clean state each time.

## CURRENT STATE (2026-07-05) — read this first

**Headline: on-device iCloud sync WORKS.** First successful push confirmed on
hardware — 3 DateBook events uploaded to the iCloud calendar (`push=3/0/0`,
`Done: +3 up`), no crashes. TLS to caldav.icloud.com now fits in RAM, login +
collection access succeed, records load and push. The whole device is a working
PalmOS-style PDA: browse/edit DateBook/Address/ToDo/Memo, Graffiti text entry,
menus/categories, and HotSync to iCloud.

**The RAM fight (this is the crux of the no-PSRAM port — don't undo it):**
TLS + WiFi + LVGL + the sync working set all have to coexist in ~85 KB of heap.
What made it fit (all in firmware/sdkconfig.defaults + bridge/sync.c):
- `CONFIG_LV_MEM_SIZE_KILOBYTES=24` (was 64 — LVGL's pool was the biggest hog).
- `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` + `DYNAMIC_FREE_CA_CERT` + `DYNAMIC_FREE_CONFIG_DATA`
  (grow the TLS RX buffer on demand; free the CA cert after handshake).
- WiFi buffers trimmed (`ESP_WIFI_STATIC_RX_BUFFER_NUM=4`, dynamic RX/TX=16).
- Sync working set: `MAXR=24`, `ARENA_CAP=8 KB` (device branch in sync.c). The
  sync holds FOUR big structs at once (S + Out in sync_collection, nodes[MAXR*2] +
  Sink in sync_one) — all scale with MAXR, so it must stay small.
- sync.c emit buffers (`body[8192]`) moved OFF the stack into one shared `g_body`
  (a per-record 8 KB stack frame overflowed the task and corrupted the heap).
- data.c seed arenas made malloc/free instead of static → reclaimed ~17 KB .bss.
- hotsync task stack = 20 KB (TLS needs it; but keep the big buffers off-stack).
- Result: **~86 KB free heap after WiFi up.** Sync peak ~75 KB. Works with margin.

**Safety net:** `sync_collection` now REFUSES to overwrite a non-empty local PDB
with an empty result (returns -2) — this stopped a real data-loss bug where an OOM
in sync_one produced 0 output and wiped the DateBook. `loadRec` also bounds-checks
the arena. Both are load-bearing; keep them.

**Graffiti (U6) is real, not a framework anymore.** Full a–z + 0–9 templates,
recognizer fixed (the killer bug was a broken `resample()` that collapsed sparse
templates; also switched to uniform scaling + no rotation-invariance). Gestures:
space = L→R swipe, backspace = R→L, **shift = upstroke (3-state: shift→CAPS
lock→off)**, **Enter = top-right→bottom-left diagonal**. Output lowercase; shift
capitalizes. Templates were hand-tuned against the official Palm chart per user
(B/D start bottom-left, E=reverse-3, G=CCW spiral+tick, M=upside-down W, N=up-down-up,
Q=top circle+tail, S=real curve, T=top-then-down). **X is a known 2-stroke
exception** (single-stroke capture can't do it yet — backlog). User: "letters good
enough for now." Stroke reference chart (rendered FROM the templates) published as
a Claude artifact: https://claude.ai/code/artifact/d8042fdf-06e2-4cb2-8b8b-1fea75fa45e9

**Other fixes landed this session:** no on-screen keyboard (Graffiti-only input);
About dialog wrap/clip fixed; **categories** fixed (older PDBs lacked AppInfo — added
a boot migration `ensure_appinfo` that backfills the category table); new DateBook
entry now defaults to today + next half-hour with editable Date/Time fields;
per-record **category assignment** via the edit-form's middle trigger button (opens
the chooser); richer HotSync diagnostics + `[sync]` stderr logging.

**Known gaps / next steps (priority order):**
1. **HotSync multi-app: code done, AWAITING FLASH.** `hotsync.c` now syncs Date
   Book + To Do + Address (each to its own collection; Address over CardDAV on the
   contacts host). Fill `SYNC_TODO_COLL`/`SYNC_CARD_COLL` in secrets.h, then flash
   and confirm on-device. Memo stays local-only (no iCloud DAV surface).
2. **ToDo lost its inline checkboxes** — lv_checkbox rows made LVGL's compositor
   loop forever behind the semi-transparent menu overlay (watchdog reset). Reverted
   to plain `[x]`/`[ ]` list buttons; "Mark Done/Undo" now lives in the ToDo detail
   view. Revisit inline check-off with a non-checkbox widget if desired.
3. **Graffiti training-game app** (BACKLOG, user-requested): launcher icon like the
   real Palm, a kanji/Chinese-writing-app-style trainer that also captures the
   user's own strokes as per-device templates. See docs/UI_ROADMAP.md backlog.
4. **MAXR cap lifted 24 → 96 (streaming engine), AWAITING FLASH** for on-device
   heap confirmation. Records now stream to/from disk (no byte arena); the only
   per-collection cap is the 96-slot index. Proven on host at device sizing
   (`make btest`). Raise MAXR further only after measuring heap on the board.
5. U8 power (battery gauge GPIO34, light-sleep), U9 case — hardware.
6. **FIXED:** `category` host-test flakiness was a wrong cleanup glob
   (`state/cat_*.map` never matched the real `state/palm_cat_*.map`); corrected.
   Use `./tests/run_gates.sh` to run the whole suite from a clean state.

**Resume in one line:**
`. ~/esp/esp-idf/export.sh && cd firmware && idf.py -p /dev/ttyUSB0 flash monitor`
(CYD on /dev/ttyUSB0). Secrets in firmware/main/secrets.h (GITIGNORED — WiFi creds +
Apple app-specific password; never commit/echo). To capture serial without the
interactive monitor: a pyserial reader to a logfile works well (see this session's
scratchpad live*.log approach); `fprintf(stderr,...)` in sync.c reaches the console.

**Asset converters:** scratchpad/{palmfont.py, palmicon.py}; PumpkinOS clone at
scratchpad/PumpkinOS. Host test gates: `make roundtrip incremental synctoken category`.

---

## U3a — convert to authentic PalmOS look   [DONE ✓]

**Source:** PumpkinOS cloned at scratchpad/PumpkinOS (github.com/migueletto/PumpkinOS,
GPLv3). Palm fonts are recreated in src/BOOT/font_90NN_72.txt as ASCII bitmaps
(GLYPH <code> + #/- rows; ascent 9 + descent 2 = 11px). 9000=stdFont(FONTID128),
9001=boldFont(129).

### Step log
- U3a.1 FONTS: scratchpad/palmfont.py converts the .txt -> LVGL v9 1bpp C font
  (pixel-exact; cell width = advance). Generated firmware/main/lv_font_palm.c
  (std) + lv_font_palm_bold.c (bold), lv_font_palm.h. Applied: screen default
  font = lv_font_palm (inherited), title bar = bold. Builds, boots. AWAITING:
  does the authentic Palm font render + is it readable?
- U3a.3 THEME: enabled CONFIG_LV_USE_THEME_MONO; lv_theme_mono_init(disp,false,
  &lv_font_palm) applied in lvgl_port -> black-on-white flat widgets w/ thin
  borders = PalmOS grayscale look. ui.c chrome: white bg, white title bar w/ black
  bold text + 2px black bottom rule, gray graffiti strip. Boots. AWAITING: does it
  read as authentic Palm now?
- U3a.1b FONT BUMP: user found 11px std too small -> regenerated lv_font_palm from
  Palm largeFont (9002, 14px), bold from largeBold (9007, 15px). palmfont.py + the
  9000-9007 .txt in scratchpad/PumpkinOS/src/BOOT.
- U3a.2 ICONS v1 (LVGL symbols) showed as EMPTY SQUARES — Palm font lacks the
  symbol glyphs. FIXED: extract REAL Palm app icons from PumpkinOS tAIB03e8.dat
  (25x22 1bpp, uncompressed, first entry of the bitmap family) via
  scratchpad/palmicon.py -> LVGL A8 images (firmware/main/palm_icons.[ch]:
  icon_datebook/address/todo/memo + a drawn icon_hotsync). Launcher rebuilt as an
  ICON GRID (flex row-wrap, 3 col, icon+label cells, recolor black) per real Palm
  Application Launcher (user wanted grid, not list). Icons render + grid + tap
  confirmed; user asked: bigger icons + home doesn't belong in title bar.
- U3a.2b: grid icons scaled 2x (lv_image_set_scale 512), cells 74x74. Removed the
  broken title-bar home (LV_SYMBOL_HOME square — Palm font has no symbol glyphs).
  Added the 4 PALM SILKSCREEN BUTTONS flanking the Graffiti area (drawn A8 icons
  silk_home/menu/find/calc in palm_icons.c): [Home][Menu] left, abc|123 center,
  [Find][Calc] right. Home->launcher; Menu = F1 entry (placeholder); Find/Calc
  placeholders. Title bar now just app title (category picker goes here in F2).
  Icon scaling (1.5x/2x) was fuzzy (non-integer A8 upscale) -> reverted to 1x crisp
  (cells 68x52, 3-across). Launcher/silkscreen DONE (commit 141eadd).

## Functional buildout F1–F4 + U6 + U7   [ALL BUILT CLEAN, awaiting one batch flash]
User: "progress through the whole build out and we'll flash and test on device at
the end." So the following were implemented + compiled clean but NOT yet flashed
(CYD /dev/ttyUSB0 was disconnected). Commits: F1 29fcefc, F2 39dd903, F3-memo
27289ee, F4 48beabc, U7 28e27ec, U6 f9ed28b.

- **F2 categories (39dd903):** demo PDBs now carry Unfiled/Business/Personal;
  title-bar category pop-up trigger filters the list (data_set_category + iterators
  honor it); Options->Categories opens it. Filter resets to All per app.
- **F3 Memo Pad (27289ee):** MemoDB = plain text (no codec); list/detail/edit
  (single textarea)/get/save/delete. All 4 apps functional now.
- **F4 Details (48beabc):** Details button on edit form -> category picker; category
  threaded through data_save_* + rewrite. FIXED bug: rewrite passed NULL AppInfo so
  edits/deletes wiped the category table -> now preserves it.
- **U7 HotSync (28e27ec):** hotsync.[ch] background task (wifi+SNTP+resolve host+
  sync_collection of the configured calendar), DEFENSIVE (no ESP_ERROR_CHECK, fails
  to a status string). HotSync app screen: Sync Now + live status. Seeding now only
  fills missing DBs (edits/synced data persist). RISK: RAM for wifi+TLS+sync while
  LVGL up (~166 free vs ~169 peak) -- may need LVGL draw-buffer teardown (mode
  switch); VALIDATE ON DEVICE.
- **U6 Graffiti (f9ed28b):** graffiti.[ch] $1 recognizer + starter templates;
  writing surface in the Graffiti strip captures strokes -> recognize -> insert into
  active field. FRAMEWORK: templates + threshold NEED on-device tuning. Fixed a
  guard collision (graffiti.h used GRAFFITI_H = display.h's strip-height macro).
- REMAINING: U8 power mgmt, U9 case (hardware). ToDo multi-column/sort polish. On-
  device tuning of Graffiti + HotSync RAM.

## F1 — menu system   [BUILT, awaiting flash]
Menu silkscreen button -> menu_open() pull-down overlay on lv_layer_top (dim
backdrop, tap-outside closes). Grouped by Palm menu categories: Record (New ->
show_edit(0); Delete -> data_delete + refresh, shown only when a record is
current via cur_uid) + Options (Categories placeholder=F2, About=lv_msgbox).
data.c gained data_delete (rewrite w/ nd==NULL drops the record; map still lists
it so sync propagates). cur_uid tracks the current record. show_edit title = New
when uid 0. Builds clean. BLOCKED: CYD /dev/ttyUSB0 vanished (USB dropped) — need
replug to flash + test. NOT committed yet (untested on HW).
- **DIRECTION SHIFT (user):** prioritize PalmOS FUNCTIONAL/menu/feature design over
  pixel fidelity. Reference = palm.wiki PalmOS Companion UI doc + PumpkinOS app
  sources (src/{DateBook,AddressBook,ToDoList,MemoPad}). New plan section "Palm
  FUNCTIONAL design" in UI_ROADMAP.md: F1 menu system (title-tap menu bar; Record/
  Options/Edit menus), F2 category picker top-right (wire existing appinfo codec),
  F3 per-app views (ToDo multi-col+sort+show-completed; DateBook day/agenda;
  Address cat filter; Memo), F4 Details dialog + Done/Details/New record buttons,
  F5 Palm controls (popup/selector triggers, checkboxes). Suggested order: F1+F2
  first, then F3 per app. This reframes the remaining UI work. NEXT: start F1/F2.
- TODO U3a.4 skin refine + LICENSE(GPLv3) (continues light/in parallel).

---

## U5 — record detail + editing   [IN PROGRESS]

**Goal:** tap a list row -> detail view; then edit forms (keyboard) that write back
to the PDB with the Palm dirty bit so changes flow through the proven sync engine.
Steps: U5.1 read-only detail views (rows carry uid -> data_detail formats fields);
U5.2 edit forms + save (set dirty bit); U5.3 new/delete. Nav: launcher->list->
detail; Home->launcher, Done->back to list. current_app tracked in ui.c.

### Step log
- U5.1: uid added to data_row_cb; data_detail(app,uid,out) formats all fields
  (cal: date/time/desc/note; addr: name/company/labeled phones/address; todo:
  pri/status/due/note). ui.c: AppDef table + cur_app, rows carry uid -> row_cb ->
  show_detail (scrollable text box + Done->list). Home->launcher. CONFIRMED by user.
- U5.2: edit forms. data.c gained data_get_* (uid->struct) + data_save_* (pack +
  rewrite PDB replacing/appending the record, sets dirty bit 0x40 -> sync uploads).
  ui.c: Edit button on detail -> show_edit; per-app form_field textareas (cal/todo:
  desc+note; addr: last/first/company/phone/note, other fields preserved via
  AddrIntern), shared lv_keyboard overlay (kill_kb on nav; shown on field tap,
  hidden on kb ready/cancel), Cancel/Save. 166KB free heap. Editing CONFIRMED by
  user (edit->save->list updates->persists to PDB).
- U5.2b: auto-scroll — on field tap, shrink form viewport to keyboard top (FORM_KB)
  + lv_obj_scroll_to_view(field); restore on kb dismiss. Confirmed by user.
  REMAINING U5: U5.3 new-record + delete (create via uid=0 rewrite already
  supported in data_save_*; needs UI: New button on list, Delete on detail/edit).

---

## U4 — read-only data views (stream from PDB on SD)   [DONE ✓]

**RESULT:** launcher apps show real records streamed from .pdb on SD. data.[ch]
seeds demo PDBs (15 events / 12 contacts / 10 todos) + iterates via pdb_read ->
codec -> display rows; ui.c list_view. SD mount fix: SPI_DMA_CH_AUTO (display
took the fixed DMA ch). Scrolling confirmed on hardware. Memo/HotSync placeholder.
NEXT: U5 record detail + editing.


**Goal:** wire launcher apps to real content from .pdb files on the SD card. User
inserted an SD card. Steps: mount SD (verify pins now that a card is present),
seed demo PDBs on-device if absent (device writes via codec), build list views
for Date Book / Address / To Do List streamed via pdb_read (Memo/HotSync stay
placeholder — no Memo codec; HotSync=U7). data.[ch] = SD seed + record iteration
as display strings; ui.c list views call it.

### Step log
- U4.1: data.[ch] (seed DatebookDB/AddressDB/ToDoDB if absent; iterate via
  pdb_read -> ApptUnpack/AddrUnpack/ToDoUnpack -> display rows). ui.c list_view +
  add_row; show_app routes Date Book/Address/To Do List to real data, Memo/
  HotSync placeholder. Format-truncation fixed w/ %.72s precision.
- U4.2: SD MOUNT FIX. spi bus init failed ESP_ERR_NOT_FOUND (display SPI3 took a
  DMA ch; SDSPI_DEFAULT_DMA is a FIXED ch that collided). SPI_DMA_DISABLED let
  bus init but block reads timed out (0x102 — SD needs DMA). Fix: SD bus init w/
  SPI_DMA_CH_AUTO -> grabs the other free DMA channel. SD MOUNTED (32GB) on HW.
  Demo PDBs seed on first boot. Views CONFIRMED showing records by user.
- U4.3: expanded demo data (15 events, 12 contacts, 10 todos) to overflow the
  visible rows; seeding now overwrites each boot (demo; U7 replaces). AWAITING:
  scroll test.

---

## U3 — app shell (LVGL, Palm-skinned)   [DONE ✓]

**RESULT:** LVGL 9.2 up on the partial-buffer path (lvgl_port.[ch]), bound to
display.c (flush) + touch.c/tp_* (indev), esp_timer tick. Palm-style shell
(ui.[ch]): navy title bar + home button, swappable content area, Graffiti strip
(abc|123). Launcher lists the classic Palm apps; tap opens a placeholder + updates
title, home returns. Confirmed on hardware (render + navigation). 189KB free heap
with LVGL. Default Montserrat font for now -> U3a swaps in authentic Palm fonts/
icons. NEXT: U4 read-only data views (stream from PDB), U3a assets.


**Goal:** LVGL on the partial-buffer path, wired to display.c (flush) + touch.c
(indev), then a Palm-style launcher + nav. Internet OK → LVGL via managed
component (lvgl/lvgl ^9.2). Steps: U3.1 integration proof (label+button, touch
works through LVGL); U3.2 launcher UI (title bar, app list, graffiti strip);
U3.3 navigation; U3a Palm font/icon assets (separate).

RAM: partial draw buffer ~240×40×2=19KB (×1), DMA-capable; U0 freed ~127KB so
plenty. flush_cb: lv_draw_sw_rgb565_swap then display_blit (need to add blit +
bump SPI max_transfer_sz). indev: touch_read -> point/state. tick: esp_timer ms.

### Step log
- U3.1: lvgl/lvgl ^9.2 managed component; display_blit (bumped SPI max_transfer_sz
  to LCD_W*60*2); lvgl_port.[ch] (partial buffer 240x40 DMA, flush=rgb565_swap+
  blit, indev=tp_read, tick=esp_timer). NAME CLASH: ESP RTC lib defines
  touch_init/touch_read -> renamed my touch API to tp_*. ui.[ch] test UI (navy
  title bar, Tap-me counter button, gray graffiti strip "abc | 123").
  Builds; LVGL up on hardware, 189KB free heap (LVGL ~67KB). CONFIRMED by user:
  UI renders + button counts on tap. LVGL integration proven. Committed d86b33d.
- U3.2/U3.3: launcher + navigation. Persistent chrome (title bar w/ home button +
  title label, content area 24..208, graffiti strip w/ abc|123 split). Launcher =
  lv_list of classic Palm apps (Date Book, Address, To Do List, Memo Pad,
  HotSync). Tap app -> placeholder view + title updates; home button -> back to
  launcher. Flashed. AWAITING: launcher renders + nav (open app / home) works?

---

## U2 — touch (XPT2046)   [DONE ✓]

**RESULT:** bit-banged XPT2046 (pins CLK25 MOSI32 MISO39 CS33; IRQ36 unused —
detect via pressure z1). Pressure threshold 110 + double-read debounce (edge
presses read weak z1 ~150; center ~800; idle <30, rare ~250 spike). 3-point
AFFINE calibration (touch_calibrate: screen = A*rawx+B*rawy+C, handles the
axis swap/flip), targets inset 40px (digitizer active area < LCD). Calibration
persisted in NVS (namespace "touch"), re-cal by holding screen at boot.
Confirmed on hardware: accurate tracking whole screen incl. former right-edge
dead zone, no idle false-touches, NVS load verified on reset. touch.[ch] in
firmware/main. Next: U3 app shell (LVGL, Palm-skinned).


**Goal:** calibrated touch input. Both HW SPI hosts are taken (SD=SPI2, TFT=SPI3),
XPT2046 is on its own pins (CLK25 MOSI32 MISO39 CS33 IRQ36; 39/36 are input-only,
no internal pulls — fine, XPT2046 drives them) → **bit-bang** the touch SPI.
Approach: (U2a) log RAW x/y/z on touch; user taps the 4 corners + center while
serial is captured → derive the affine screen map from real data. (U2b) apply
map, draw a tracking dot, user confirms; store cal (hardcode first, NVS later).
Build flag U2_TOUCH_TEST gates the touch test loop vs the normal wifi/sync flow.

### Step log
- U2.1: bit-bang XPT2046 reader + raw-logging loop.
- U2.2: FIX — IRQ pin (GPIO36) never signaled touch; switched to PRESSURE
  detection (z1>300; idle z1<251, touched z1~700-970). Also fixed bit-bang:
  12-bit read was misaligned (was read16>>4; now skip busy bit + read 12) and
  swapped X/Y command bytes (X=0xD0, Y=0x90). Touch now reads.
- U2.3: calibrated from corner taps. AXES SWAPPED: screenX<-rawY (301..3663),
  screenY<-rawX (~3700 top..~570 bottom, flipped). cal in touch.c:
  swap=1, xmin=301 xmax=3663 flipx=0, ymin=570 ymax=3700 flipy=1.
  Switched to tracking-dot build. Result: horizontal finger drag produced
  VERTICAL dot motion — blind corner clustering was unreliable (BL/BR
  mis-registered). Abandoned manual swap/flip guessing.
- U2.4: replaced with GENERAL AFFINE calibration (touch_calibrate): draws 3
  crosshair targets (TL/TR/BL), user taps each, solve3() solves
  screen = A*rawx + B*rawy + C per axis (handles swap/flip/scale/skew in one).
  Applied in touch_read. app_main runs touch_calibrate() then tracking dot.
  Flashed. Tracking + accuracy CONFIRMED good by user.
- U2.5: user reports a wedge-shaped dead zone on the RIGHT edge (~1/6 wide at
  top-right tapering to ~1/20 at bottom-right). Diagnosed w/ raw probe: NOT a
  dead panel — right-edge presses DO register (rawY up to 3788) but z1
  (pressure) reads only ~145-196 there vs ~800 center; the 300 threshold
  rejected them. Resistive panels read lower pressure near edges (divider
  geometry). Idle z1 mostly <30, rare spikes ~250 (overlaps edge presses).
  FIX: TOUCH_Z1_MIN 300→110 + double-read debounce in touch_pressed; avg_touch
  uses same. Re-flashed. AWAITING: does the dead zone shrink + any idle false
  touches at threshold 110?

---

## U1 — display bring-up (ILI9341/ST7789)   [DONE ✓]

**RESULT:** ILI9341 works in **portrait 240×320** on SPI3 @ 20 MHz (pins SCLK14
MOSI13 DC2 CS15 BL21), MADCTL 0x48, no inversion. User confirmed colors +
orientation correct. Layout zones defined: PDA area 240×208 on top, Graffiti
input strip 240×112 on the bottom (with letters|numbers split). display.[ch]
in firmware/main. Next: U2 touch (XPT2046).


**Goal:** get pixels on the 2.8" 320×240 panel via a dependency-free SPI driver
(no managed components), draw a diagnostic test pattern, confirm panel type +
color order + orientation. Verification needs the USER's eyes (no camera here).

Plan/pins: TFT on **SPI3** (SD is on SPI2 — no bus conflict). Pins SCLK14 MOSI13
DC2 CS15 RST-1(software reset) BL21. Driver in firmware/main/display.[ch]:
init + fill_rect via a one-row buffer (RGB565). Test pattern = 4 colored
quadrants (TL red / TR green / BL blue / BR white) + yellow border, so the user
report disambiguates: color order (RGB vs BGR = red/blue swap), inversion
(negative colors), orientation (which quadrant is where), and addressing
offset (border complete?).

### Step log
- U1.1: wrote display.[ch] (ILI9341, SPI3, pins 14/13/2/15/BL21), test pattern.
- U1.2: FIX crash — 40MHz rejected (TFT pins route via GPIO matrix on SPI3, cap
  ~26.7MHz). Dropped to 20MHz. Panel init + test pattern now run clean (serial
  confirms). AWAITING user's on-screen report (colors/orientation/border).
  Tweakables in display.c if wrong: MADCTL_VAL, INVERT, PANEL_ST7789.
- U1.3: user requires PORTRAIT (Palm PDA on top, Graffiti strip bottom). Switched
  to 240x320 (LCD_W/H), MADCTL 0x48, GRAFFITI_H=64/PDA_H in display.h. New test
  pattern previews the layout: blue title bar + gray PDA body (top), gray Graffiti
  strip (bottom), RED dot TL / GREEN dot TR for orientation, yellow border.
  Flashed clean. Panel confirmed working in portrait (user could identify regions).
- U1.4: user feedback "graffiti area too small". Bumped GRAFFITI_H 64→112 (~35%,
  PDA_H=208), added Palm-style letters|numbers vertical divider in the strip.
  Re-flashed. AWAITING confirm on size + colors/orientation to close U1.

---

## U0 — sync working set: static BSS → heap   [DONE ✓ d7031ed]

**RESULT (measured on hardware):** static DRAM 174 KB→47 KB (−127 KB); boot free
heap **129 KB → 256 KB** (+127 KB), largest block 110→127 KB. All 5 host gates
green. This is the UI budget unlocked — ~256 KB free in UI mode; sync buffers are
malloc'd during a sync and freed after, so they don't tax interactive mode.


**Goal:** the big sync buffers are `static` (permanently resident in BSS). With
`--gc-sections`, the resident ones today are `sync_collection`'s `S`+`Out` and
`sync_one`'s `nodes[]`+`Sink` (~123 KB). Making them heap (malloc-on-sync /
free-after) hands that ~123 KB back to UI mode; peak-during-sync is unchanged
(they're needed then). Both host and device convert to heap (identical behavior;
gates prove it). Verify: all 5 host gates green + firmware BSS drops + boot heap
rises.

Targets in `bridge/sync.c`:
- [x] `sync_one`: `static Node nodes[MAXR*2]` + `static Sink kbuf` → heap (+free)
- [x] `sync_collection`: `static S s` + `static Out o` → heap (+free)
- [x] `sync_categorized`: `static S all` + `static Out o` + `static S sub` → heap
- [x] `sync_pull`: `static uint8_t arena[ARENA_CAP]` + `static PdbRec recs[MAXR]`
      → heap (and fix `sizeof arena` → `ARENA_CAP`); also NameList → heap
- [x] add alloc-failure handling (log + graceful return)
- [x] host: `make test itest stest ctest` + `dav_roundtrip.sh` all green
- [x] device: `idf.py size` BSS delta + flash + boot free-heap delta
- [x] commit + push

### Step log
- U0.1 DONE: sync_one nodes[]+Sink → calloc/free, alloc-fail guard. `make itest` green.
- U0.2 DONE: sync_collection S+Out → calloc/free (returns -1 on OOM). itest+stest green.
- U0.3 DONE: sync_categorized all+o+sub → calloc/free (sub reused via memset in loop). ctest green.
- U0.4 DONE: sync_pull arena+recs+NameList → heap, sizeof→ARENA_CAP. dav_roundtrip green.
- U0.5 DONE: full host sweep green (test 285/0, itest, stest, ctest, dav_roundtrip).
- U0.6 DONE: firmware static DRAM 174K→47K; boot heap 129K→256K on hardware.

---

## Status of everything else (as of U0 start)
- Phase A (contacts) + Phase B (ESP32 port) DONE, on hardware; discovery smoke
  test proven vs live iCloud (commits through f7bac41).
- UI decision: LVGL skinned with real Palm assets + PumpkinOS layouts; firmware
  GPLv3 (357f044). Roadmap: docs/UI_ROADMAP.md.
- Next after U0: U1 display bring-up (ILI9341/ST7789), then U2 touch.
