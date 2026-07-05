# Build progress log (durable — survives context loss)

Running log of the UI build (docs/UI_ROADMAP.md). Updated after each step so work
can resume cold. Newest phase on top.

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
