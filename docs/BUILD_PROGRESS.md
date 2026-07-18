# Build history — changelog + hard-won lessons

What was built, and the non-obvious things that cost time to learn. This is the
**historical record**; for **what's left to do see `BACKLOG.md`**.

> The full blow-by-blow (every step log, raw serial captures, dead-end diagnoses)
> lives in git history — this file was compacted on 2026-07-17. `git log --follow
> docs/BUILD_PROGRESS.md` recovers the long-form entries if a detail is needed.

---

## Milestone changelog (newest first)

### 2026-07 — RSS reader (roadmap #4) stage B: the reader app
- **`bridge/news.c`** — the on-SD article store: `news.idx` (a header + fixed
  172-byte records: feed, title, blob offset+len, time) + `news.dat` (bodies
  concatenated). The reader seeks one record + one body span per article, so
  browsing is O(1) RAM regardless of store size; the writer streams articles in
  with `news_begin`/`news_add`/`news_commit`. Host-gated (`tests/news_test.c`:
  write, reopen, read back meta + bodies, truncation, rewrite) in `make test`.
- **"News" launcher app** (`show_news`, its own newspaper icon) — one article per
  screen: feed · `n/N` · **bold title** · body (clipped feed-card style). Navigates
  by **vertical swipe**: since the headless host doesn't reliably synthesize LVGL's
  velocity-based gesture, swipe is detected manually from the press Y vs. the last
  PRESSING Y (the RELEASED event's indev point is already reset to 0) — which also
  works on the real resistive panel. Seeded with sample articles until a real fetch
  runs. Pool-safe (labels + content swap on one clickable surface; sim heap peak 0).
- The launcher is now **seven apps → three rows**; the I1.1 onboarding hint moved
  into the grid's flex flow (a full-width row after the icons) so it no longer
  overlaps row 3. `news.c` + `rss.c` compile into both the sim and firmware builds.
  Smoke-gated (launch News → swipe up → next article).

### 2026-07 — RSS reader (roadmap #4) stage A: the feed parser
- Feasibility settled (GO): fetch **streams to SD** with bounded per-item RAM (the
  DAV sliding-window pattern), sync stays short with a feed/item cap + conditional
  GET, and the reader holds only the current article + a small index in RAM — no
  whole-feed or all-articles buffer. The live HTTPS fetch is device-only (stubbed
  in the sim), but the parsing is portable and host-gated.
- **`bridge/rss.c`** — a streaming RSS 2.0 / Atom parser: a byte-driven state
  machine accumulates only the current `<item>`/`<entry>` into a bounded buffer,
  then extracts `<title>` and the richest body (content:encoded → description →
  content → summary). `rss_html_to_text` strips tags + decodes entities in one
  pass, crucially treating `&lt;`/`&gt;` as tag delimiters so it handles both CDATA
  raw-HTML and the entity-escaped HTML that RSS `<description>` usually carries.
  Numeric + named entities → UTF-8. Host-gated (`tests/rss_test.c`: RSS + Atom,
  CDATA vs escaped, body preference, item cap, file vs buffer) in `make test`, plus
  a sanitized `rss_asan` in `ftest` (it eats untrusted network bytes).

### 2026-07 — Graffiti trainer: finish item #2 (training mode, graded score, icon)
- **Training mode.** A Drill/Train toggle. Train mode records the user's *own* stroke
  for each letter as a **per-device template** — `graffiti_capture_user()` grabs the
  raw stroke (via a new pre-recognition `graf_capture_hook` so the buffer is intact),
  downsampled to ≤32 points, ~3.3 KB in BSS, persisted to `/sdcard/graf_user.dat` and
  loaded at boot in `ui_init`. `graffiti_recognize` now also matches user templates
  (`match_user`) and prefers one when it's a closer fit than the built-in — calibrating
  recognition to the user's hand + this resistive panel.
- **Graded score.** `graffiti_last_distance()` exposes the $1 match distance; Drill mode
  shows a quality `%` (0 = perfect, LET_THRESH = just-accepted): "Nice! 81%".
- **Launcher app.** Promoted from a menu item to a first-class launcher app with its
  own 25×22 icon (`icon_graffiti`, a bold "A"); the Options-menu shortcut was removed.
  The launcher reflowed to 2×3, so the smoke's Memo/HotSync taps moved with it.
- Smoke-gated: launch via icon → Drill graded score → Train toggle → capture
  ("saved your 'a'", recorded 1/26). `make -C sim smoke` + `graf` gate green.

### 2026-07 — Graffiti SRS trainer (roadmap #2, v1)
- New **Graffiti Trainer** app (Menu → "Graffiti Trainer", `show_trainer`): shows a
  target letter + its **stroke guide** — drawn on a 96×96 I1 canvas from the
  recognizer's own template (`graffiti_letter_template`) with a filled start dot for
  direction. You write in the Graffiti strip; a new `graf_char_hook` in `graf_up_cb`
  routes the recognized char to the trainer instead of a textarea (the hook is
  cleared by `kill_kb`, so it can't outlive the screen). Scoring uses the real
  recognizer; a **per-letter Leitner box** (0..4) schedules the next glyph (weight
  5−box, weak letters resurface more) and persists to `/sdcard/graf_train.dat`.
  Correct → "Nice!" + advance + streak; wrong → shows what it read, same target.
- **Feasibility (roadmap's explicit ask) proven by construction:** pool-safe
  (labels + one canvas, no layer widget; sim heap peak 0), and stroke capture reuses
  the proven text-entry pipeline. Smoke-gated (`trainer` / `trainer_scored`: draw
  the fresh-state first target 'm' → "Nice!"). Remaining: a training mode that
  captures the user's own strokes as per-device templates, a launcher icon, graded
  scoring.

### 2026-07 — Graffiti recognizer polish
- **Accuracy harness + CI gate** (`sim/tests/graf_test.c`, `make -C sim graf`).
  Synthesizes hand-drawn-like strokes from each template (polyline densified +
  deterministic Gaussian jitter at a realistic pixel scale), runs them through the
  real `graffiti_recognize()`, and reports per-glyph accuracy + top confusions;
  fails CI if accuracy drops below the gate (letters/digits, mean ≥95%, every
  glyph ≥80%). Test-only accessors (`graf_test_*`, `#ifdef GRAF_TEST_HOOKS`) expose
  the templates without duplicating the tables.
- **Template separation fixes** guided by the harness: `h` (rounded n-hump, was
  colliding with `k`), `g` (circle + hooked descender, was `o`/`q`), `p` (long
  stem + compact top bowl, was `d`). Letters **97.5% → 99.6%** mean at 3 px jitter
  (worst glyph 72% → 92%); digits stay 100%. The synthetic model is a proxy —
  final thresholds still want real on-device `graf` telemetry.
- **Punctuation coverage** added to the harness (arming the punct-shift with a tap,
  then the stroke). All 8 punct glyphs (`@ , / - ' ( ) ?`) recognize at 100% — the
  set was already well-separated (orientation distinguishes the straight lines), so
  no punct template edits were needed; the gate now locks all three sets.

### 2026-07 — review cycle: charm + intuitiveness in the simulator
- **I4 config-save feedback** — the Preferences field editor (silent) and the
  "Save to config.ini" row (modal alert) now show the same transient "Saved" toast
  as record save/delete, so config edits close the feedback loop consistently. A
  *write failure* stays a modal alert (the user must notice the card didn't take).
- **C4 event Alarm + Repeat** — the Date Book Details sheet gained an Alarm on/off
  toggle and a Repeat cycle (None/Daily/Weekly/Monthly/Yearly), both plain buttons
  that relabel in place. The `Appt` struct + `ApptPack`/`ical_emit` already carried
  `hasAlarm`/`hasRepeat` (VALARM/RRULE), so this was UI-only: `g_ev_*` init from the
  loaded event in the edit-form builder, applied in `save_cb`. Round-trip verified
  in the sim (set On/Weekly -> save -> reopen shows On/Weekly). Smoke-gated.
- **C4 Address edit form** — expanded from 5 to 10 scrollable fields (Last, First,
  Title, Company, Phone, Address, City, State, Zip, Note). The `form_field` builder
  order and `save_cb`'s `fv()` indices move in lock-step; fields the form still
  doesn't expose (phone2-5 labels, country, custom1-4) are preserved from the old
  record on save. Smoke-gated (scroll to reveal the lower fields).
- **C4 Edit Categories** — the category picker's tail row ("Edit Categories") opens
  a pool-safe editor: a list of the app's categories (Unfiled reserved/hidden), a
  **New** button, and per-category **rename** via the I1.2 tap keyboard (one
  textarea + one button-matrix). `data_set_categories` writes the new AppInfo back
  with `pdb_write_ai`, preserving every record (a record's category nibble is
  untouched, so a rename retags its whole category). Delete is deferred (needs
  recategorising affected records to Unfiled). Smoke-gated.
- **I3** — Date Book **Week view**; Day/Week/Month zoom hierarchy (centre label
  zooms out, day tap zooms in).
- **I4** — transient save/delete **toasts** (were silent-success). **C6** — About
  states the Memo (device-only) / To Do (CalDAV tasks, not Reminders) truths.
- **I2** — demo seed is manifest-tracked; **"Remove demo data"** deletes exactly
  the seeded rows (uniqueID 1..nr, one rewrite per app) before a first HotSync can
  push them to the real iCloud. **I1.1** — launcher onboarding hint until an
  account is configured.
- **C7** — authentic **inverted (white-on-black) Palm title bar**; whole-`ui.c`
  sweep confirmed no draw-layer widget remains.
- **Brightness freeze fixed** — the Preferences brightness row was an `lv_slider`
  (== an `lv_bar` == a draw-layer alloc that live-locks the 24 KB pool → WDT).
  Replaced with a pool-safe `[-] NN% [+]` stepper. See lessons below.
- **S4 charm sprint (sim)** — C1 Graffiti ink trail + char echo, C2 HotSync
  dialog, C4 Palm form contract (Done/Details/Delete bottom bar), I1.2 Preferences
  on-screen keyboard, C5 devtools gating. All in the real `ui.c`, smoke-gated.
- **The simulator (S0–S3)** — the real `firmware/main/ui.c` builds to a **native
  headless** host (scripted input + PPM/PNG screenshots, a CI smoke gate) and to
  **WASM** for the phone browser, live on GitHub Pages
  (https://emu-commits.github.io/cyd-palm-bridge/). A device-like general-heap
  ceiling (linker-wrapped malloc) + IDBFS `/sdcard` persistence + credential
  scrubbing round it out. Plan: `SIMULATOR_PLAN.md`.

### 2026-07-15 — product-hygiene sprint + CI + whole-repo review
- Legal: **GPLv3/MIT split + NOTICE** (PumpkinOS provenance, Palm trademark).
- **CI** (GitHub Actions): host codec/fuzz gates, Radicale sync gates, ESP-IDF
  `idf.py build`, and later the simulator smoke + wasm jobs + Pages deploy.
- Newcomer-facing **README**; **M1** sync scratch buffers BSS → sync-lifetime heap
  (~20 KB returns to interactive mode); host `[sync]` telemetry behind `SYNC_DEBUG`;
  M4 discovery cap + O6 detail buffer micro-fixes. The review itself is
  `REVIEW_2026-07-15.md` (source of the C#/I#/M# item IDs).

### 2026-07-10 — sync robustness + on-device UX
- **iCloud modern Reminders are walled off from CalDAV** (Apple, since iOS 13 /
  Catalina) — unfixable by any CalDAV client. **Decision:** To Do stays on the
  iCloud **CalDAV VTODO lane** (still cloud-backed; view on iPhone by adding iCloud
  as an external CalDAV account).
- **Drift self-heal** — incremental sync could orphan a record whose first pull
  failed (the RFC 6578 sync-token advanced past it). Device now **always
  full-enumerates and never persists a sync-token** — a full reconcile every sync
  self-heals. Host keeps the incremental fast path.
- **Streaming enumeration** — the 8 KB response buffer truncated a ~42 KB REPORT →
  collection skipped. `dav_sync_report`/`dav_list` now spool to SD + sliding-window
  parse; enumeration RAM is O(1) in record count. 8 KB cap **gone** (verified on
  device, 64-event Date Book). Gate: `tests/streamparse.c`.
- **TLS keep-alive** — reuse one `esp_http_client` connection per origin
  (handshake was per-request → ~2N handshakes to pull N records).
- On device: **light-sleep disabled** (it flashes this CYD's display), screen
  **stays awake during sync**, To Do **due-date picker**, live **brightness slider**
  (later found to freeze — see the 2026-07-16 fix), U8 **PWM backlight**, finer
  per-record sync progress (text), To Do due-date sort, **relocation idempotency**
  (defer unresolvable objects, suppress that round's deletes; gate
  `tests/idempotent.c`), Preferences persistence bug (FATFS `rename`), record
  sorting, **Find UI**, top-bar clock, **TZ picker + DST**, `lv_table`
  virtualization (the `LIST_MAX` cap is gone), Address Look Up / To Do checkbox
  column / Memo first-line list.

### 2026-07-09 — sync correctness + on-device config
- **Sync is correct end-to-end on device**: map frozen by FATFS `rename` (→
  `remove`+`rename`); push keeps/maps only on 2xx; 412 duplicate-UID conflicts
  resolved; the OOM-wipes-DateBook data-loss guard (`sync_collection` refuses to
  overwrite non-empty local with empty).
- **UID-based identity** (iCal/vCard UID hash, not href-derived) — kills the
  relocation / dup-UID class. Gate `tests/uidmatch.c`. **Streaming reconcile** —
  disk-backed 3-way merge-join, O(1) RAM, `MAXR` cap gone. Gate `tests/bigsync.c`.
- **On-device `config.ini`** — runtime load (`appcfg.[ch]`), Preferences editor UI,
  and Discover (walk iCloud CalDAV + CardDAV homes, assign collections to roles).
- **Graffiti punctuation** — PalmOS shift (a tap arms "PUNC", next stroke matched
  against `PTMPL`; period = two taps).

### 2026-07-05 → earlier — first device sync, then the whole PDA
- **First on-device iCloud push confirmed** (3 events, no crash) after the no-PSRAM
  RAM fight (see lessons). Hardware-less sessions added Find, Calculator, parser
  hardening, engine streaming, multi-app HotSync.
- The PalmOS UI, built step by step on hardware: **U1** display (ILI9341 portrait),
  **U2** touch (XPT2046, calibrated, NVS), **U3/U3a** LVGL app shell + Palm
  fonts/icons/theme, **U4** data views, **U5** detail + edit, **F1–F4** menus /
  categories / Memo+apps / Details, **U6** Graffiti (full a–z/0–9 recognizer),
  **U7** HotSync (bidirectional iCloud), per-record Delete + confirm, Calculator.
- **U0** — sync working set static BSS → heap (`d7031ed`), the prerequisite that
  freed the RAM for everything above.
- **Phase A/B** (`ROADMAP.md`, done): CardDAV contact sync, then the ESP32
  firmware port of the exact codec + sync engine.

---

## Hard-won lessons (durable — re-read before touching these areas)

### LVGL on a 24 KB object pool (`CONFIG_LV_MEM_SIZE_KILOBYTES=24`)
- **Never use a widget that allocates a draw LAYER.** `lv_bar` (and its subclass
  `lv_slider`), `lv_arc`, `lv_meter` composite their indicator through a layer
  buffer allocated from the fixed pool. On this pool the alloc fails, LVGL retries
  every refresh, IDLE0 starves, the Task WDT fires → **frozen screen**. This bit us
  three times (sync progress bar at 66%, To Do inline `lv_checkbox` behind the menu
  overlay, Preferences brightness slider). **Use plain buttons / button-matrix /
  lists / labels** — the Calculator + keyboard + list pattern never allocates a
  layer. Progress and steppers are **text**, not bars.
- **`bg_opa`/`bg_color` (a background fill) does NOT force a layer.** Only object
  `opa`, transforms, or blend modes do. So `LV_OPA_30` popup backdrops and a
  tinted "today" row are safe.
- **LVGL 9.2 `lv_table` tap** clears the selected cell on RELEASED *before*
  `LV_EVENT_CLICKED` fires — read the cell on `LV_EVENT_VALUE_CHANGED` instead.
- **Keep big buffers off the task stack.** A per-record 8 KB stack frame overflowed
  the hotsync task and corrupted the heap; emit buffers moved to one shared `g_body`.

### The no-PSRAM RAM fight (don't undo `sdkconfig.defaults` / `sync.c` sizing)
- TLS + Wi-Fi + LVGL + the sync working set must coexist in ~85 KB. What makes it
  fit: `LV_MEM_SIZE=24 KB`, `MBEDTLS_DYNAMIC_BUFFER` + free-CA-after-handshake,
  trimmed Wi-Fi buffers, and the sync set kept small (`MAXR=24`, `ARENA_CAP=8 KB` —
  `S` + `Out` + `nodes[]` + `Sink` all scale with `MAXR` and must coexist with the
  mbedTLS handshake). Result: ~86 KB free after Wi-Fi up, sync peak ~75 KB.
- **Time-multiplex, don't sum:** rich UI and TLS never run at the same instant.
  Wi-Fi is down during interactive use; the screen shows only a status line during
  a sync. `dav_disconnect()` frees the TLS working set before every heap-heavy sort.
- **Stream everything.** PDB I/O is one record at a time; DAV enumeration + reconcile
  are disk-backed sliding-window / merge-join. Watch the **window-boundary bug**: a
  `<response>` straddling the parse window must keep its tail, or a record is dropped
  (`dav_parse_*_stream`).

### Sync / iCloud specifics
- **iCloud CardDAV is on a different host** than CalDAV (`contacts.icloud.com`, not
  `caldav.icloud.com`) — contact discovery must start there.
- **iCloud namespaces every XML element** `xmlns="DAV:"`; the parser is prefix-tolerant
  (`xml_open`/`xml_text`).
- Auth is an **app-specific password** (with dashes). Never commit/echo/log the
  password fields (`secrets.h` is gitignored; `appcfg.h` marks Config SENSITIVE).
- **No RTC** → **SNTP on Wi-Fi connect** before any HTTPS, or TLS cert validity fails.

### Hardware (this CYD, ESP32-2432S028R, no PSRAM)
- **Display** ILI9341 (some units ST7789), **portrait 240×320**, SPI3 @ **20 MHz**
  (pins route via the GPIO matrix on SPI3, cap ~26.7 MHz — 40 MHz crashes init);
  pins SCLK14 MOSI13 DC2 CS15 BL21, MADCTL 0x48. Layout: PDA area 240×208 top,
  Graffiti strip 240×112 bottom.
- **Touch** XPT2046, **bit-banged** on its own pins (CLK25 MOSI32 MISO39 CS33; IRQ36
  unused — detect via **pressure z1**, not IRQ). 3-point **affine** calibration
  (handles axis swap/flip/skew in one solve), stored in NVS ("touch"), re-cal by
  holding at boot. Pressure threshold **110** (resistive panels read weak pressure
  near edges — a 300 threshold created a right-edge "dead zone").
- **Light-sleep flashes the display** on this board (APB clock gates between LVGL
  frames) — disabled; PWM backlight + idle screen-off are the power story instead.

### Simulator
- **Pool scaling is pointer-width sensitive.** 24 KB assumes 32-bit pointers; an
  LP64 native host needs ~48 KB for the same object capacity. The **wasm build is
  32-bit** → true 24 KB parity with the device (trust wasm over the native host for
  memory-limit reproduction).
- **General malloc is capped too** (`sim_heap.h` + `heap_budget.c`, linker-wrapped)
  so the browser's unlimited memory doesn't mask the device budget.
- **Credentials are never persisted to browser storage** — `sim_scrub_config()`
  blanks the password fields before every IDBFS write (sync is stubbed; stored
  creds would be pure risk). The public page tells users not to enter real creds.
- **`data_delete()` returns 1 on success** (`rewrite() >= 0`), not 0 — batch
  deletes must count `>= 1`, and per-record rewrites are O(n²) SD churn (batch them).

---

## Resume / reference

```
# device
. ~/esp/esp-idf/export.sh && cd firmware && idf.py -p /dev/ttyUSB0 flash monitor
# secrets in firmware/main/secrets.h (GITIGNORED: Wi-Fi + Apple app password)

# host gates
make roundtrip incremental synctoken category   # codec + sync
./tests/run_gates.sh                             # full suite from a clean state (Radicale)

# simulator
make -C sim smoke     # native headless + screenshots (CI gate)
make -C sim wasm      # browser build
```

- **Graffiti stroke chart** (rendered from the templates):
  https://claude.ai/code/artifact/d8042fdf-06e2-4cb2-8b8b-1fea75fa45e9
- **Asset converters:** `scratchpad/{palmfont.py, palmicon.py}`; PumpkinOS clone at
  `scratchpad/PumpkinOS`.
- Known Graffiti gap: **X is a 2-stroke exception** single-stroke capture can't do
  yet (backlog).
