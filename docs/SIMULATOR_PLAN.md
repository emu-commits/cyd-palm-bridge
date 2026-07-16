# Next phase — a mobile-friendly UI simulator

> **STATUS (2026-07-16): S0–S3 BUILT** (`sim/`). The real `ui.c` boots headless
> and renders correctly (screenshot-verified: launcher, Date Book Day view,
> Address list/detail with live Look Up filtering, To Do, Memo, HotSync screen,
> menus — and the `$1` recognizer resolving a Graffiti stroke end-to-end into a
> field). `make -C sim smoke` is a CI gate with screenshots uploaded per push;
> `make -C sim wasm` builds the browser version in CI (emsdk container). Two
> deltas from this plan as written: **32/64-bit pool scaling** (24 KB assumes
> 32-bit pointers; an LP64 host needs 48 KB for the same object capacity — the
> wasm build is 32-bit and keeps true 24 KB parity), and the **native headless
> frontend** (scripted input + PPM/PNG screenshots) was added as the local dev
> loop + CI gate alongside the browser target. Pages is enabled (Source:
> GitHub Actions) -- the sim publishes to
> https://emu-commits.github.io/cyd-palm-bridge/ on every push.
>
> **UPDATE (same day, post-launch):** two gaps found by using it on a phone,
> both fixed. (1) **General-heap ceiling** (`sim/sim_heap.h` + `heap_budget.c`,
> linker-wrapped malloc/calloc/realloc/free): the LVGL pool was already exact,
> but general malloc drew from unlimited browser memory -- now it's capped at a
> device-like budget (default 144 KB ~= the UI_ROADMAP Mode A free heap), armed
> after boot; verified by a 1 KB-budget build rendering the UI's real
> "(low memory)" degrade path. (2) **Persistent SD card**: /sdcard is now an
> IDBFS (IndexedDB) mount -- records/prefs survive reloads, per-browser and
> origin-isolated. Credentials are deliberately NOT persisted: sim_scrub_config()
> blanks the password fields of config.ini before every write to browser
> storage (session-only in RAM; sync is stubbed so stored creds would be pure
> risk).
>
> **S4 (2026-07-16, part 2): the charm sprint is BUILT in the sim** -- C1 ink
> trail + char echo, C2 HotSync dialog, C4 form contract, I1.2 Preferences
> keyboard, C5 devtools gating -- all in the real ui.c, screenshot-verified,
> covered by the smoke gate.
>
> **S4.1 (part 3-4):** a Preferences->Brightness **freeze** found on the live sim
> was fixed (an `lv_slider` == an `lv_bar` == draw-layer alloc -> WDT; replaced
> with a pool-safe `[-] NN% [+]` stepper, now a smoke gate). Then **C7 title bar**
> adopted the authentic inverted (white-on-black) Palm look; a whole-`ui.c` sweep
> confirmed no other layer-alloc widget remains, and C7 untimed-first Day view was
> verified already-correct. Remaining: C7 ✓-glyph (font regen), C3 sound
> (hardware), the on-glass verify, and **S5** (fetch-based sync).

Goal: run the real Palm UI (`firmware/main/ui.c`) **in a phone browser**, so the
look-and-feel and the review's UX-charm backlog can be built and reviewed from a
phone — no board, no flashing, no cable — and so the no-PSRAM LVGL memory limits
reproduce off-device. This is the "yellow bucket" enabler from
`docs/REVIEW_2026-07-15.md` §6: once it exists, C1 (Graffiti ink), C2 (HotSync
dialog), C4 (Palm form contract), I1.2 (on-screen keyboard), and M2 (sync-mode
LVGL teardown) become iterate-in-minutes work instead of flash-and-pray.

> **Why mobile-friendly matters here:** the whole point is to make progress and get
> design sign-off while away from the bench. A desktop-only SDL window doesn't do
> that; a WebAssembly build served as a static page does — tap the actual Palm on
> your phone, anywhere.

---

## The key architectural win (why this is tractable)

`ui.c` is already platform-clean: it includes **only project headers** (`ui.h`,
`display.h`, `data.h`, `lv_font_palm.h`, `palm_icons.h`, `hotsync.h`, `graffiti.h`,
`calc.h`, `find.h`, `appcfg.h`, `power.h`, `clock.h`) plus `lvgl.h` and libc — **no
`esp_*`, no FreeRTOS, no driver includes**. So the UI compiles unchanged for the
browser; only the *modules behind those headers* need a browser implementation. The
seam already exists — we just add a second backend.

What each dependency needs for the web target:

| Header / module | On device | In the simulator |
|---|---|---|
| `lvgl_port.c` (display + touch) | ILI9341 + XPT2046, partial buffers | LVGL's SDL/Emscripten driver, full canvas |
| `data.[ch]` | PDBs on `/sdcard` via bridge codec | same codec, PDBs in the Emscripten FS (MEMFS/IDBFS); `data_seed_if_empty()` already seeds demo data on boot |
| `graffiti.c` | `$1` recognizer + `esp_log` | **compiles as-is** once `esp_log.h`→`printf` is stubbed (pure math) |
| `calc.c`, `find.c` | already in `bridge/`, host-clean | reuse verbatim |
| bridge codec (`pdb/datebook/address/todo/appinfo/ical/vcard/tz/charset/dav_xml`) | IDF component | reuse verbatim (already builds on host) |
| `hotsync.[ch]` | Wi-Fi + TLS + sync task | **stub** first (HotSync screen shows "sync disabled in simulator"); real sync is a stretch goal (S5) |
| `clock.[ch]` | SNTP + NVS checkpoints | stub: browser `Date`, TZ table stays (pure) |
| `power.[ch]` | LEDC backlight + `esp_pm` | stub: brightness = a no-op or a CSS filter |
| `appcfg.c` + `config.c` | `config.ini` on SD | `config.c` is host-clean; store `config.ini` in the browser FS |

Everything except `lvgl_port`, `hotsync`, `clock`, `power` is already portable. The
platform shim is a handful of small files under a new `sim/` directory.

---

## Approach — the real WASM simulator (single track)

Compile the actual `ui.c` + its portable dependencies to WebAssembly with
Emscripten, behind the `sim/` shim. This is a genuine interactive Palm in the
browser: real navigation, real records, real Graffiti recognition, real Calculator
and Find — the thing you iterate the UX in.

> A throwaway HTML/CSS mockup was considered and **dropped as redundant**: the port
> renders the real `ui.c`, so a separate hand-built replica would just be duplicate
> UI to maintain. Design/charm decisions are made directly in the real sim.

---

## Build & delivery

- **Toolchain:** Emscripten (`emscripten/emsdk` container) → `emcc`. Build LVGL
  from the same `^9.2` source the firmware pins, with a `sim/lv_conf.h` that mirrors
  the firmware's relevant settings — crucially **`LV_MEM_SIZE = 24 KB`** so the LVGL
  object-pool exhaustion class (the `lv_list`/`lv_bar` crashes documented in `ui.c`)
  reproduces in the browser, plus the mono theme and the Palm fonts.
- **Display:** LVGL SDL driver under Emscripten renders to a `<canvas>`. The sim uses
  a **full** framebuffer (fine in a browser) — see "What it does/doesn't validate".
- **Storage:** preload/seed demo PDBs into MEMFS on boot via the existing
  `data_seed_if_empty()`; optionally IDBFS to persist edits across reloads.
- **Output:** a self-contained `index.html` + `.js` + `.wasm`.
- **Mobile-friendly delivery:**
  - Responsive canvas: 240×320 portrait scaled with CSS to the viewport
    (`width:min(100vw, 62.5vh)` aspect-locked, integer-scale when it fits); a
    `<meta viewport>`; `touch-action:none` on the canvas so taps don't scroll/zoom.
  - Touch → LVGL pointer (Emscripten SDL maps touch to mouse; add a small touch
    handler if press/drag fidelity needs it — Graffiti strokes need continuous move
    events).
  - Optional charm: a Palm Pilot **bezel** image framing the canvas (makes the
    portrait aspect natural on a wide phone and is itself a charm win).
  - **Host it live:** a GitHub Pages deploy job so every push updates a public URL
    you can open on your phone. (One-off design shares can also go out as Artifacts.)

---

## Phased build

- **S0 — Seam audit (½ day).** Enumerate every non-portable symbol in `ui.c`'s
  dependency graph (table above is the start); create `sim/` with stub headers so a
  link failure lists exactly what's missing. Deliverable: a shim skeleton.
- **S1 — LVGL-in-browser hello (½–1 day).** LVGL `^9.2` + SDL/Emscripten driver +
  `sim/lv_conf.h` (mono theme, `LV_MEM_SIZE=24K`, Palm fonts) rendering a 240×320
  test screen in a `<canvas>`. Proves the toolchain end-to-end.
- **S2 — Boot the real UI (1–2 days).** Compile `ui.c` + `data.c` + the bridge codec
  + `graffiti.c` + `calc.c` + `find.c` + stubs (`hotsync`/`clock`/`power`); seed demo
  PDBs into MEMFS; reach the launcher and navigate list → detail → edit; Graffiti
  writes into fields; Calculator + Find work. **First interactive Palm in a browser.**
- **S3 — Mobile polish + live deploy (½–1 day).** Responsive scaling, touch mapping,
  viewport, optional bezel; GitHub Pages CI job builds the WASM and publishes it.
  Now tappable from a phone at a stable URL.
- **S4 — Build the charm backlog in the sim.** With the loop fast, land the review's
  UX items: **C1** Graffiti ink trail (LVGL canvas over the strip), **C2** HotSync
  progress dialog (logo + conduit lines, no `lv_bar`), **C4** Palm form contract
  (Done/Details/New bottom row; OK-right/Cancel-left), **I1.2** on-screen keyboard
  fallback for password fields (the calc button-matrix pattern), plus **C5/C7**
  polish. Each is verified in-browser, then flashed once to confirm on glass.
- **S5 — (stretch) real sync in the browser.** Wire `hotsync` to `fetch()` against a
  CORS-enabled test DAV (a local Radicale with CORS, or a tiny proxy) so the sim can
  actually HotSync. Lets M2 (sync-mode teardown) be prototyped against real timing.

---

## What the simulator validates — and what it doesn't

**Validates:** UI structure and navigation, Palm authenticity/charm, LVGL widget +
layout behavior, the **object-pool exhaustion class** (with `LV_MEM_SIZE=24K`),
Graffiti recognition logic, and the record/codec data path (same `bridge/` code).

**Does NOT validate:** the ILI9341 partial-render/DMA/flush path, touch calibration
on the resistive panel, real heap headroom during Wi-Fi+TLS, light-sleep/backlight,
or Graffiti *thresholds* against the real panel. Those stay on the "needs hardware"
list — the sim narrows what a flash session must check, it doesn't replace it.

---

## CI / repo shape

- New `sim/` dir: `lv_conf.h`, the SDL/Emscripten `lvgl_port` backend, and stubs
  (`hotsync_stub.c`, `clock_stub.c`, `power_stub.c`, `esp_log`→`printf` shim), plus a
  `Makefile`/`CMake` for `emcc`.
- New CI job `simulator-build` (`emscripten/emsdk` image) that compiles the WASM on
  every push — a compile gate for `ui.c` on the second backend, catching UI breakage
  the firmware job's headless link wouldn't. Optional Pages-deploy step behind it.
- Keep it **off the critical path** of the existing firmware/host CI jobs.

## Risks / mitigations

- **LVGL SDL/Emscripten parity** — pin the same LVGL version + a mirrored `lv_conf`;
  accept the full-framebuffer difference (documented above).
- **Shim drift** — keep the shim thin; the `simulator-build` CI job fails loudly when
  `ui.c` grows a new dependency, which is the signal to add a stub.
- **Touch fidelity for Graffiti** — validate continuous-move capture early (S1/S2);
  add an explicit touch handler if SDL's mouse mapping drops points.
- **Scope** — hotsync/network is deferred to S5 so S2–S4 need no backend, keeping
  the first interactive build small.

---

## Definition of done (phase)

A public, phone-tappable URL where the real Palm UI boots to the launcher, browses
and edits demo records, takes Graffiti input, runs the Calculator and Find, and
reproduces the LVGL pool limits — updated automatically on every push — with the
first charm items (C1 ink, C2 HotSync dialog) built and reviewed in it.
