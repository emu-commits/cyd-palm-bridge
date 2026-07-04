# Build progress log (durable — survives context loss)

Running log of the UI build (docs/UI_ROADMAP.md). Updated after each step so work
can resume cold. Newest phase on top.

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
