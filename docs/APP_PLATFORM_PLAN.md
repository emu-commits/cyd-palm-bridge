# App platform plan — native C apps from the SD card

> **Status: PROPOSAL.** Nothing here is built. The decisions in §11 were
> made on 2026-09-27; the v1 API list in §5 is proposed and awaits review.
> This plan follows the
> MicroPython-versus-native measurement round (QEMU, ESP-IDF v5.5, this
> firmware's own image). Its conclusion was: **compiled C apps, loaded from
> the SD card**, with the firmware exporting a small, stable, versioned API.
> This document plans how to do that, how to split app development from the
> firmware's build and versions, and how to check an app before it reaches
> the device.

## Contents

1. [Goals and non-goals](#1-goals-and-non-goals)
2. [What the measurements fixed](#2-what-the-measurements-fixed)
3. [Architecture](#3-architecture)
4. [Splitting apps from the firmware: repos, versions, compatibility](#4-splitting-apps-from-the-firmware)
5. [The app SDK (`palm_app.h`)](#5-the-app-sdk)
6. [Palm C: the language profile, written for AI authors](#6-palm-c-the-language-profile)
7. [The inspection process](#7-the-inspection-process)
8. [Guards on the device](#8-guards-on-the-device)
9. [Threat model](#9-threat-model)
10. [Phases and exit criteria](#10-phases-and-exit-criteria)
11. [Decisions](#11-decisions-made-2026-09-27)

---

## 1) Goals and non-goals

**Goals**

- **New apps without reflashing.** An app is a file on the SD card. Adding,
  updating or removing one never touches the firmware.
- **Separate versioning.** Firmware releases and app releases happen on their
  own schedules. A firmware update does not break installed apps unless the
  API's major version changes, and the device says so when it does.
- **AI-authorable.** An AI with tools (like a Claude Code session) can write,
  build, test and package an app on its own. A chat-only AI can write the
  source, and a GitHub Action does the rest.
- **Inspected before it runs.** Before a pack goes on the card, one command
  puts it through a fixed series of memory-safety and behaviour checks and
  prints a plain-text report of any issues (§7). There is no signing; the
  inspection is a manual step.

**Non-goals (v1)**

- **Running untrusted third-party apps safely.** The ESP32 has no hardware
  memory protection that could isolate an app from the firmware (§9). The
  checks catch *bugs*, not malice. Flash encryption is ruled out (§11);
  instead the API is designed to limit what an app can reach (§8.2).
- **Network access from apps.** Wi-Fi + TLS needs more than 100 KB of heap,
  which can't sit beside an app.
- **Apps building their own LVGL widget trees.** The LVGL pool has about
  11 KB free with the launcher showing. Apps draw on the shared 1-bpp canvas
  and use firmware-built controls.
- **Moving the built-in games out of the firmware.** They stay in C in the
  image. We can revisit this once the API has been stable for a while
  (§10, "Later").

---

## 2) What the measurements fixed

These numbers constrain the design. Each one came from the QEMU round unless
it is marked as an estimate.

| Fact | Value | Consequence |
|---|---|---|
| Free heap, launcher showing | 114,352 B (largest block 90,112 B) | About 106–108 KB on hardware once the SD card is mounted (estimate). |
| Free IRAM (code-only RAM) | **30,720 B** largest block | App code goes here **at no cost to the data heap**. App `.text` budget: **24 KB**, with the remainder kept as headroom. |
| Static DRAM | 89 % used, 19.9 KB left | The app host adds no large static buffers. App data is allocated on the heap when the app opens. |
| LVGL pool free, launcher showing | 11,024 B | Apps get one canvas and a fixed set of controls, all created by the firmware. |
| Shared game canvas | `game_cv_buf`, 240×164 at 1 bpp (≈5 KB, `ui.c`) | Apps draw into this buffer. Only one game, app or Coach sigil is on screen at a time, which already makes sharing safe. |
| App image budget | 1.58 MB of the 3 MB `factory` partition | Flash isn't a constraint. The loader's size is still to be measured (Phase 0). |
| Speed | C Sudoku board: 9.5 ms (Python: 2.19 s) | No performance workarounds are needed in the SDK. |
| LVGL runs on | the **main task** (`lvgl_port.c`, 16 KB stack, about 3.5 KB spare at the `ui_init` peak) | Apps run inside LVGL callbacks on this stack, with no task of their own. Each app gets a **2 KB stack budget**, proven statically (§7, stage G2). |
| Secrets | Wi-Fi and iCloud passwords in NVS, **not encrypted** (`secretstore.h`), **and copied into RAM for as long as the device is on** (`g_cfg` in `appcfg.c`, then into stack buffers in `hotsync.c` that are never wiped) | Native code could read them. The API-side protections (§8.2) and the binary scan in the inspection (§7, G6) cover this; the first of those protections takes the passwords out of resident RAM. |
| ESP32 IRAM access | 32-bit aligned access only | Only `.text` (and literals) can go in IRAM. `.rodata`, `.data` and `.bss` must go in DRAM. Byte loads from IRAM fault. |
| Xtensa LX6 | Unaligned 32-bit loads raise `LoadStoreAlignment` | UBSan's alignment check runs in the inspection (G4). |

---

## 3) Architecture

```
 SD card                             Firmware (factory partition)
 /sdcard/apps/                       ┌──────────────────────────────────────────┐
   dice/                             │ ui.c ── "More" folder (was "Games")      │
     app.pack  ──── read, check ───▶ │   built-ins: Mines Wordie Sudoku Zip …   │
     data/…   ─── read-only ───────▶ │   packs:     one tile per installed app   │
     notes.txt ◀── pa_file_* API ──  │                                          │
     state.bin ◀─ pa_state_* API ──  │ apphost.c  check → load → run → unload   │
   crash.log                         │   ├─ packfmt.c   header + SHA-256        │
                                     │   ├─ loader      ELF relocation (IRAM)   │
                                     │   └─ guards      canaries, crash flag,   │
                                     │                  time/stack watermarks   │
                                     │ palm_api.c  the pa_api table (v1.x)      │
                                     │   (compiled into the simulator too)      │
                                     └──────────────────────────────────────────┘
```

**Life of an app, on the device:**

1. The **More** folder lists the built-in games plus each `/sdcard/apps/*/app.pack`.
   To draw a tile it reads only the pack's 256-byte header (name, icon,
   API version).
   Nothing is loaded yet.
2. On tap, `apphost` **checks** the pack: magic, format version, API
   compatibility, size limits, and the SHA-256 (which catches a truncated or
   corrupted copy). It also refuses an app that is in quarantine (§8). It
   doesn't check whether the pack was inspected; that's the user's step (§7).
3. It **loads** the pack. Code and literals go to IRAM (≤ 24 KB). Read-only
   data, data and bss go to one heap block with guard words at both ends
   (≤ 16 KB by default). It applies the relocations and frees the ELF buffer.
4. It **runs** the app. It builds the app's `pa_api` table (with functions for
   capabilities the app wasn't granted pointing at "denied" stubs), calls
   `pa_entry(api)`, and gets back the app's descriptor. From then on every
   LVGL event on the app's canvas, buttons and menu becomes one call to the
   app's `on_event`, which runs on the LVGL task. Guards run around each call (§8).
5. It **unloads** the app on close, Home, or a Coach seal. It sends
   `PA_EV_CLOSE` (the app saves its state), frees IRAM and the data block,
   and checks that the heap and LVGL pool are back to where they were.

**Two binary-interface decisions, with the reasons:**

- **A function table instead of linking against firmware symbols.** The app's
  only import is the `pa_api` pointer it is given. The inspection's binary scan (G6) can
  then require **zero undefined symbols** (apart from a short list of compiler
  helpers the firmware exports: `memcpy`, `memset`, `memmove`, and 64-bit
  division/shift helpers). The table is also what versioning appends to, and
  what capabilities are enforced through. The header hides it behind inline
  wrappers, so apps call `pa_text(...)`, not `api->text(...)`.
- **Relocatable ELF, loaded by Espressif's `elf_loader` component** (lists
  plain ESP32 as supported). If Phase 0 finds its flash cost or its ESP32
  IRAM handling unacceptable, the fallback is a small custom loader. With a
  function table, an app only needs about four Xtensa relocation types.

**Firmware files (new):** `apphost.c/.h`, `packfmt.c/.h`, `palm_api.c`, plus
`sdk/palm_app.h`. `ui.c` changes only at the Games→More folder and at one
hook that hands the content area, canvas buffer and button strip to
`apphost`. That keeps this work out of the 10.6k-line file, in line with the
`ui.c` split proposed in `BACKLOG.md`.

---

## 4) Splitting apps from the firmware

### 4.1 Repositories (decided: in this repo until the API freezes)

**Until `sdk-v1.0`**, apps live in **this repo, under `apps/`**, next to
`sdk/`. They are kept separate from the firmware in every way except the
repo:

| Path | Holds | Licence | CI | Releases |
|---|---|---|---|---|
| `firmware/`, `sim/`, `bridge/` | As today | As today (firmware GPLv3) | `ci.yml`, as today | `vX.Y.Z` tags |
| `sdk/` | `palm_app.h`, linker script, build rules, `mkpack`, `packlint`, `stackcheck`, `inspect.sh` and its container image, the ABI freeze file, `docs/` | **MIT** (§11) | `ci.yml` (ABI check) plus `apps.yml` | `sdk-vA.B` tags once frozen |
| `apps/` | One folder per app, `AGENTS.md`/`CLAUDE.md` for AI authors, the examples | **MIT** (§11) | **`apps.yml`**, triggered only by changes under `apps/**` or `sdk/**`, running the inspection on changed apps | **`app-<id>-vX.Y`** tags → a release carrying the `.pack` and its `inspect-report.txt` |

Two rules keep the later split cheap:
- **Nothing in `apps/` includes anything outside `sdk/`.** `packlint`
  enforces it (G0), so an app never picks up a firmware header by accident.
- **`apps.yml` builds the SDK from `sdk/` and the simulator from the same
  commit**, exactly as the external repo will do from a pinned tag.

**At `sdk-v1.0`** (Phase 5), `apps/` moves to a new template repo,
**`cyd-palm-apps`**, with its history (`git filter-repo --subdirectory-filter
apps`). It gets a `palm-sdk.lock` file naming the `sdk-vA.B` tag, and its CI
checks out this repo at that tag for the SDK, simulator and tools. Moving an app to a newer SDK then becomes a
one-line PR that bumps the lock file.

### 4.2 Three version numbers, and what each one promises

| Version | Lives in | Bumped when | Promise |
|---|---|---|---|
| **Firmware** `vX.Y.Z` | git tag → `PROJECT_VER` → `esp_app_get_description()` | Any release | None to apps directly. Shown in Settings ▸ About with the API range it serves. |
| **API** `A.B` | `sdk/palm_app.h`: `PA_API_MAJOR`, `PA_API_MINOR` | `B` when slots are **appended** to `pa_api` or new event types are added; `A` when anything is removed, reordered or changes meaning | Firmware serving `A.B` runs every pack built for `A.b` with `b ≤ B`. |
| **App** `X.Y` | the app's `PA_APP(...)` descriptor → pack header | The app author's choice | Its state file carries its own version (`pa_state_load` rejects a mismatch, and the app decides whether to migrate). |

**Load rule:** `pack.api_major == fw.api_major && pack.api_minor <= fw.api_minor`.
If that fails, the tile shows "Needs newer firmware" (or "Built for an older
system"), and the app isn't loaded.

### 4.3 Keeping the API stable (CI gates in this repo)

- **ABI freeze file.** `sdk/abi/pa_api_v1.txt` lists every `pa_api` slot in
  order with its prototype. A gate (`make -C sdk abicheck`) regenerates the
  list from the header and fails if an existing line changed or moved.
  Appending lines is allowed, but only if `PA_API_MINOR` went up. Removing or
  changing a line requires a new major version and a new freeze file.
- **Struct layout checks.** `palm_api.c` asserts `offsetof` and `sizeof` for
  every public struct at compile time for Xtensa, so a layout drift fails the
  firmware build, not a user's app.
- **App compatibility gate.** On any PR that touches `sdk/`, `palm_api.c` or
  `apphost.c` (and nightly), CI builds every app at its latest `app-*` tag
  (after the split: every app in `cyd-palm-apps` at its released tag), then
  runs their tours against the new firmware in the simulator. This gate is what makes the split safe: a
  firmware change can't quietly break an installed app.
- **Deprecation, not deletion.** A slot that is no longer wanted keeps working
  and is marked `PA_DEPRECATED` until the next major version.

### 4.4 The pack file

```
offset  size  field
0       8     magic "PALMPAK\x01"
8       2     pack format version (1)
10      2     api_major, api_minor (1 byte each)
12      32    app id (reverse-DNS, NUL-padded)   e.g. "com.cbalinskas.dice"
44      24    display name (NUL-padded)
68      4     app version (major<<16 | minor)
72      4     capability bits (v1: PA_CAP_FILES only — see §5.3)
76      4     text size   (≤ 24 KB)
80      4     data+bss size (≤ 16 KB, or as granted)
84      4     ELF length
88      32    SHA-256 of the ELF
120     8     reserved (0)
128     66    tile icon: 24×22, 1 bpp, rows padded to 3 bytes (§5.6)
194     62    description, UTF-8, NUL-padded (§5.6)
256     …     ELF (relocatable, Xtensa)
```

The More folder draws a tile from the first 256 bytes alone.

- **No signature.** The SHA-256 is there to catch a truncated or corrupted
  copy, and to give the pack an identity: the inspection report (§7.4) and
  **About <app>** on the device both show it, so you can confirm the pack on
  the card is the one you inspected. It proves nothing about who built it.
- The pack format lets the device check the file cheaply before loading
  anything.

---

## 5) The app SDK

One header, `palm_app.h`, is the whole API. The **v1 list below is the
proposal for review.** The first draft covered what the four built-in games
use today:
- a 1-bpp canvas with pen down, drag and up;
- two or three status labels;
- one or two buttons ("New", "Dig/Flag");
- the Palm font in regular and bold;
- a play clock;
- a save file.

The 2026-09-27 review added what a general app developer would want next:
- wrapped text;
- a grey pattern;
- a list picker;
- a text prompt;
- region scrolling;
- a clean self-stop;
- the user's preferences;
- a tile icon, and rules for data files.

Anything only *possibly* useful waits for 1.1, because once `sdk-v1.0` is
frozen each slot has to be kept for good.

### 5.1 Conventions

- **Canvas:** 240×164 pixels, 1 bpp, with the origin at the top left.
- **Colours:**
  - `PA_PAPER` and `PA_INK`.
  - `PA_XOR`, the Palm-style selection highlight, so no separate `invert`
    call is needed.
  - `PA_GRAY`, a 50 % checkerboard (the stipple Mines and Coach already
    draw). The checkerboard is aligned to canvas coordinates, so neighbouring
    grey fills join without a seam. `pa_clear`, `pa_fill` and `pa_disc`
    accept it; the other calls draw it as `PA_INK`.
- **Every drawing call clips to the canvas.** The firmware redraws the canvas
  after each `on_event` in which something was drawn, so there is no
  `invalidate` call for an app to forget.
- **Errors:** functions that can fail return `pa_err` (`int32_t`). They
  never fault.

  | Code | Meaning |
  |---|---|
  | `PA_OK` = 0 | Success |
  | `PA_E_ARG` | Bad argument, including a pointer outside the app's own memory (§8.2) |
  | `PA_E_DENIED` | The app lacks the capability, or tried to write to `data/` |
  | `PA_E_NOTFOUND` | No such file, preference or list entry |
  | `PA_E_NOSPACE` | Over a quota |
  | `PA_E_LIMIT` | Too many of something |
  | `PA_E_IO` | The SD card failed |
  | `PA_E_BUSY` | A picker or prompt is already open |

- **Text is UTF-8 in, Palm font out** (decided 2026-09-27). Apps pass UTF-8
  strings. The firmware draws them in the device's own 11 px Palm font,
  folding every character to plain ASCII first:
  - Accented Latin letters become the base letter: `é → e`, `Ç → C`, `ñ → n`,
    `ü → u`.
  - Ligatures expand: `æ → ae`, `œ → oe`, `ß → ss`.
  - Curly quotes become straight quotes, en and em dashes become `-`, `…`
    becomes `...`, and a non-breaking space becomes a space.
  - Anything else, and any invalid UTF-8, becomes `?`.

  The fold table sits beside the CP1252 tables in `bridge/charset.c`, so the
  simulator draws exactly what the device draws. It applies to every string
  that reaches the screen: text, labels, status, alerts, pickers and prompts.
  Width and wrapping are measured on the folded text. `maxlen` arguments
  count **input bytes**. Text returned by `pa_prompt_text` is already plain
  ASCII.
- **The title bar belongs to the firmware.** It always shows the pack's
  display name, so an app can't make itself look like Settings or a password
  prompt (§8.2). That's why there is no `pa_title()`.

### 5.2 Events

`on_event(const pa_event *e)`, with `pa_event = { uint8_t type; uint8_t id;
int16_t x, y; uint32_t ch; }`:

| Event | Fields | When |
|---|---|---|
| `PA_EV_OPEN` | — | Once, after load. Restore state and draw here. |
| `PA_EV_CLOSE` | — | Once, before unload: Home, the app calling `pa_exit`, a HotSync starting, or a Coach seal. Save state here. There are no more events after it. **It isn't sent after `pa_fail`.** |
| `PA_EV_PEN_DOWN` / `PEN_MOVE` / `PEN_UP` | `x`, `y` in canvas coordinates | Touches on the canvas. `PEN_MOVE` is sent at most every 20 ms. |
| `PA_EV_BUTTON` | `id` 0–3 | A tap on one of the app's buttons. |
| `PA_EV_MENU` | `id` 0–5 | One of the app's menu items. |
| `PA_EV_CHAR` | `ch`: ASCII, `'\b'`, `'\n'` | A Graffiti stroke, while the Graffiti strip is on. |
| `PA_EV_TICK` | — | The app's timer. It doesn't fire while the screen is off. |
| `PA_EV_CONFIRM` | `id` as passed to `pa_confirm`; `x` = 1 for yes, 0 for no | The answer to a confirmation. |
| `PA_EV_PICK` | `id` as passed to `pa_pick`; `x` = the chosen index, or −1 if cancelled | The answer to a list picker. |
| `PA_EV_PROMPT` | `id` as passed to `pa_prompt`; `x` = 1 for OK, 0 for Cancel | The prompt closed. On OK, read the text with `pa_prompt_text`. |

While a picker, prompt, alert or confirmation is open, the app gets no pen,
button, menu or character events. Only one can be open at a time
(`PA_E_BUSY`).

### 5.3 The proposed v1 table (slot order is the ABI)

★ marks calls added in the 2026-09-27 review. Nothing is frozen yet, so the
slots were renumbered to keep related calls together.

| # | Call | Contract |
|---|---|---|
| | **Meta** | |
| 0 | `uint16_t pa_api_version(void)` | `(major << 8) \| minor`, as served by the firmware. |
| | **Canvas** | |
| 1 | `void pa_clear(uint8_t colour)` | Fill the whole canvas. |
| 2 | `void pa_pixel(int16_t x, int16_t y, uint8_t colour)` | |
| 3 | `void pa_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t colour)` | |
| 4 | `void pa_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t colour)` | Outline. |
| 5 | `void pa_fill(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t colour)` | Filled. `PA_XOR` gives the selection highlight and `PA_GRAY` the stipple. |
| 6 | `void pa_disc(int16_t cx, int16_t cy, int16_t r, uint8_t colour)` | Filled circle, r ≤ 80. |
| 7 | `pa_err pa_blit(const uint8_t *bits, int16_t w, int16_t h, int16_t x, int16_t y, uint8_t colour)` | 1-bpp bitmap, rows padded to bytes, MSB first. Set bits are drawn in `colour`; clear bits are left alone. Reads exactly `((w+7)/8)*h` bytes, all of which must be in app memory. |
| 8 ★ | `void pa_scroll(int16_t x, int16_t y, int16_t w, int16_t h, int16_t dx, int16_t dy, uint8_t fill)` | Shift the pixels in a rectangle by `dx`, `dy`. The strip uncovered by the shift is filled with `fill`, and pixels shifted out are dropped. It's for smooth-scrolling lists and readers: shift, then draw only the new strip. |
| 9 | `int16_t pa_text(int16_t x, int16_t y, uint8_t font, const char *s, uint16_t maxlen, uint8_t colour)` | `font` is `PA_FONT_STD` or `PA_FONT_BOLD` (the 11 px Palm fonts). Draws at most `maxlen` bytes on one line and returns the width drawn. |
| 10 ★ | `int32_t pa_text_box(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t font, const char *s, uint32_t maxlen, uint8_t colour)` | Word-wrapped text in a box. Lines break at spaces and at `'\n'`; a word wider than the box is split. It stops at the last line that fits completely and **returns the number of input bytes it drew**, so the next page starts at `s + result`. |
| 11 | `int16_t pa_text_width(uint8_t font, const char *s, uint16_t maxlen)` | For centring and alignment. |
| 12 | `uint8_t pa_font_height(uint8_t font)` | |
| | **Screen furniture** (drawn by the firmware in Palm style) | |
| 13 | `pa_err pa_buttons(const char *const *labels, uint8_t n)` | 0–4 buttons in the bottom strip, each label ≤ 10 bytes. `n = 0` removes them. |
| 14 | `pa_err pa_button_label(uint8_t id, const char *label)` | Relabel one button, for toggles like Dig/Flag, without rebuilding the strip. |
| 15 | `void pa_status(uint8_t slot, const char *s)` | Slot 0 is left-aligned and slot 1 right-aligned, on the line above the buttons. ≤ 24 bytes each. |
| 16 | `pa_err pa_menu(const char *const *labels, uint8_t n)` | 0–6 app items at the top of Menu. The firmware always adds "About <app>" (§5.6). |
| 17 | `void pa_alert(const char *s)` | Modal OK box, ≤ 160 bytes. |
| 18 | `void pa_confirm(const char *s, uint8_t id)` | Yes/No box. The answer arrives as `PA_EV_CONFIRM`. |
| 19 ★ | `pa_err pa_pick(const char *title, const char *const *labels, uint8_t n, int16_t selected, uint8_t id)` | A Palm-style pop-up list of 1–32 items, each ≤ 24 bytes, scrolling when longer than the screen. `selected` highlights one item (−1 for none). The answer arrives as `PA_EV_PICK`. The firmware copies the labels, so the app's array needn't outlive the call. |
| 20 ★ | `pa_err pa_prompt(const char *title, const char *initial, uint8_t maxlen, uint8_t kind, uint8_t id)` | A modal box for typing up to `maxlen` bytes (≤ 64), with the device's own Graffiti and on-screen keyboard. `kind` is `PA_PROMPT_TEXT` or `PA_PROMPT_NUMBER` (digits, `-` and `.` only). Typed text is always shown, never masked (§8.2). The result arrives as `PA_EV_PROMPT`. |
| 21 ★ | `int32_t pa_prompt_text(char *buf, uint16_t cap)` | After `PA_EV_PROMPT` with OK: copies the typed text (ASCII, NUL-terminated, truncated to `cap`) and returns its length. The text is kept until the next prompt opens. |
| 22 | `pa_err pa_graffiti(uint8_t mode)` | `PA_GRAF_OFF`, `PA_GRAF_LETTERS` or `PA_GRAF_DIGITS`. Shows the Graffiti strip; strokes arrive as `PA_EV_CHAR`. |
| | **Time** | |
| 23 | `int64_t pa_now(void)` | UNIX seconds, or 0 if the clock has never been set. |
| 24 | `pa_err pa_localtime(int64_t t, pa_tm *out)` | In the device's time zone. `pa_tm` holds year, month, day, hour, minute, second and weekday. |
| 25 | `uint32_t pa_ticks_ms(void)` | Monotonic milliseconds since boot. |
| 26 | `pa_err pa_timer(uint16_t period_ms)` | One repeating `PA_EV_TICK`. 0 stops it; the minimum is 100 ms. |
| 27 | `uint32_t pa_random(void)` | Hardware RNG. For reproducible boards, seed the header-only `pa_rng` from it. |
| | **User preferences** ★ | |
| 28 ★ | `int32_t pa_pref(uint16_t key)` | A numeric setting, or `PA_E_NOTFOUND` for a key this firmware doesn't know. v1 keys: `PA_PREF_CLOCK24` (1 = 24-hour clock, from the existing `clock24` setting), `PA_PREF_WEEK_START` (0 = Sunday, 1 = Monday; the firmware has no such setting yet, so this returns 0 until Settings gains one), `PA_PREF_BATTERY_LOW` (1 when the power gauge says low). Unknown keys return an error instead of failing, so new keys can arrive in minor versions without new slots. |
| 29 ★ | `int32_t pa_pref_str(uint16_t key, char *buf, uint16_t cap)` | A text setting, copied NUL-terminated and returning its length. v1 key: `PA_PREF_OWNER` (the owner's name from Settings, as shown on the lock screen). Passwords, accounts, Wi-Fi networks and location aren't available through any key. |
| | **State** | |
| 30 | `int32_t pa_state_load(void *buf, uint32_t len, uint16_t ver)` | Returns `len` on success. If the file is missing, is a different size or version, or fails its CRC, it returns an error and **leaves `buf` untouched**. |
| 31 | `pa_err pa_state_save(const void *buf, uint32_t len, uint16_t ver)` | Crash-safe write (`safefile`) to `state.bin`, ≤ 4 KB. Can be called at any time, not only on close. Apps should save after any change worth keeping, because a battery pull sends no `PA_EV_CLOSE`. |
| | **Files** (`PA_CAP_FILES`; see §5.7 for the folder rules) | |
| 32 | `int32_t pa_file_open(const char *name, uint8_t mode)` | `PA_READ`, `PA_WRITE` (truncates) or `PA_APPEND`. A name is either `file` (the app's own files) or `data/file` (read-only); `file` matches `[a-z0-9._-]{1,31}`. At most 2 open at once. Returns a handle ≥ 0. |
| 33 | `int32_t pa_file_read(int32_t h, void *buf, uint32_t len)` | Bytes read, 0 at the end of the file. |
| 34 | `int32_t pa_file_write(int32_t h, const void *buf, uint32_t len)` | Bytes written. `PA_E_NOSPACE` if the app's own files would exceed **256 KB in total**. |
| 35 | `pa_err pa_file_seek(int32_t h, uint32_t pos)` | |
| 36 | `int32_t pa_file_size(int32_t h)` | |
| 37 | `pa_err pa_file_close(int32_t h)` | The firmware closes any handles still open on unload. |
| 38 | `pa_err pa_file_remove(const char *name)` | The app's own files only; `data/` gives `PA_E_DENIED`. |
| 39 | `int32_t pa_file_list(uint8_t where, uint16_t index, char *name, uint16_t cap)` | The n-th file in `PA_DIR_OWN` or `PA_DIR_DATA`, or `PA_E_NOTFOUND` past the end. Only names that follow the rules are listed (§5.7). |
| | **Memory** | |
| 40 | `void *pa_arena(uint32_t bytes)` | One zeroed block per session, ≤ 24 KB, freed on unload. A second call returns NULL. |
| | **System** | |
| 41 | `void pa_log(const char *s)` | One line to the serial console (and the simulator's stdout), ≤ 80 bytes, at most 10 lines a second. |
| 42 ★ | `void pa_fail(const char *msg)` | **Stops the app now**, for a state it knows is wrong. It doesn't return and sends no `PA_EV_CLOSE`, so a bad state is never saved. The screen shows "<app> stopped: <msg>", and one line goes to `crash.log`. It isn't a crash, so the app isn't quarantined. The header's `PA_ASSERT(cond)` calls it with the file, line and condition. In the inspection, reaching `pa_fail` during any run is an **ERROR** that quotes the message. |
| 43 | `void pa_exit(void)` | Ask to close. `PA_EV_CLOSE` follows after the current event returns. |

That's **44 slots** (8 added in the review). There are no capabilities
besides `PA_CAP_FILES` in v1. The capability field in the pack header is 32
bits, so later ones (such as `PA_CAP_PIM_READ`) have room.

**How `pa_fail` stops the app.** `apphost` sets a `setjmp` point before each
call into the app, and `pa_fail` jumps back to it. That is safe here because
Palm C code has no destructors or held locks, and the firmware releases the
app's files, arena and IRAM on the way out, the same as a normal unload.

**Compiler helpers the loader resolves by name** (not in the table; the G6
allow-list): `memcpy`, `memset`, `memmove`, `memcmp`, and the 64-bit integer
helpers (`__divdi3`, `__udivdi3`, `__moddi3`, `__umoddi3`, `__ashldi3`,
`__ashrdi3`, `__lshrdi3`, `__muldi3`). Single-precision `float` runs on the
ESP32's FPU and needs no helper. `double` would pull in soft-float helpers,
so it is not allowed (§6.1).

**Header-only helpers** (compiled into the app, not part of the ABI, and free
to improve between SDK versions): `pa_strlcpy`, `pa_strlcat`, `pa_fmt_int`,
`pa_fmt_time` (`m:ss` / `h:mm:ss`, honouring `PA_PREF_CLOCK24` when given a
time of day), `pa_min`/`pa_max`/`pa_clamp`, `PA_COUNTOF`, `PA_ASSERT`,
`pa_rng` (seeded xorshift32, so generators are reproducible and host-testable
like `sd_new`), and `playclock.h` (unchanged from the firmware).

### 5.4 Budgets (checked by the inspection, enforced by the loader and guards)

| Resource | Limit |
|---|---|
| Code (`.text` and literals, in IRAM) | 24 KB |
| Data (`.rodata`, `.data` and `.bss`, in DRAM) | 16 KB. Large tables such as word lists go in `data/` as files. |
| Arena | 24 KB, one block |
| Stack, worst case | 2 KB, no recursion |
| Time per event | 50 ms (G7), with a device watermark at 100 ms (§8) |
| State file | 4 KB |
| The app's own files | 256 KB in total |
| `data/` | Read-only; no quota |
| Open files | 2 |
| Buttons / menu items / status slots | 4 / 6 / 2 |
| Picker | 32 items, 24 bytes each |
| Prompt | 64 bytes |

With an app at its maximum (16 + 24 KB of data), about **66 KB** of the
~106 KB heap stays free for the system. That's more than the 48 KB-heap
MicroPython option would have left.

### 5.5 Deferred to 1.1 or later (not in v1 on purpose)

| Deferred | Why not now |
|---|---|
| A large-digit font (the lock screen's `DASH_DIG`), circle outlines, polygons | Apps can draw them with `pa_blit` and `pa_line`. Add them once two apps have needed them. |
| A text-field widget on the canvas | `pa_prompt` covers typing a value, and `PA_EV_CHAR` plus `pa_text` covers live entry. A widget costs LVGL pool and fixes a look before any app has asked for one. |
| Keeping the screen awake (`pa_keep_awake`) | Considered in the review and not chosen for v1. Timer and reader apps go dark at the idle timeout. |
| A backlight pulse for attention (`pa_attention`) | Considered in the review and not chosen for v1. |
| Reminders that fire while the app is closed | Needs the firmware to own a schedule and a lock-screen notice. |
| Write-only Memo and Date Book entries | Lower risk than reading PIM data, but still needs a consent design. |
| A long-press event | Apps can build one from pen events and `pa_ticks_ms`. Add one if several apps do. |
| Sleep/wake events | Play clocks already work from `pa_now()`. |
| A second timer, a sub-100 ms timer | No game needs it; it costs battery. |
| Read-only PIM access (`PA_CAP_PIM_READ`) | Needs a consent prompt design, and a decision on what an app may see. |
| Per-app settings in Settings | An app's own menu covers it. |
| Sound | No hardware yet (`PRODUCT_PLAN.md` §4). |
| Networking, NVS, raw LVGL, threads | Out of scope (§1). |

### 5.6 The tile icon and description

- **Icon:** `icon.png` in the app's source folder, **24×22 pixels, black
  and white only** (black is ink). That's the size of the built-in launcher
  icons in `palm_icons.c`. `mkpack` packs it into the header as 66 bytes of
  1-bpp bitmap. The More folder expands it to the same 8-bit mask the
  built-in icons use, recoloured the same way, so pack tiles match built-in
  ones. That costs 528 bytes of heap per tile on screen.
- **Description:** one line in `app.toml` (`description = "…"`), ≤ 61 bytes,
  UTF-8 folded like any other text. **About <app>** shows it, with the id,
  version, API version, capabilities and the first 12 characters of the
  pack's SHA-256.
- **Missing or wrong:** a missing icon gets a generic tile and a
  **WARNING** from the inspection. An icon that is the wrong size or has grey
  pixels is an **ERROR** (G0), because the result on the device would be
  unpredictable.

### 5.7 The app's folder, and data files

```
/sdcard/apps/<app-id>/
  app.pack        the app (header, icon, code)        written by you; the app can't open it
  state.bin       pa_state_save / pa_state_load       firmware-managed; the app can't open it
  notes.txt …     the app's own files                  read/write, 256 KB in total
  data/           files that ship with the app, or    READ-ONLY to the app, no quota
    words.txt     that you copy there (books, lists)
```

- **Installing** means copying the inspection's output folder, which holds
  `app.pack` plus the app's `data/`, to `/sdcard/apps/<app-id>/`.
  **Uninstalling** means deleting that folder.
- **`data/` is read-only to the app.** It holds shipped files (from the
  app's source folder) and anything you add yourself, such as a book for a
  reader app. Writing, removing or creating files there returns
  `PA_E_DENIED`, so an app bug can't damage them. They don't count against
  the quota.
- **Names:** lower-case letters, digits, `.`, `_` and `-`, 1–31 characters,
  and no subfolders inside `data/`. FAT on the card ignores case, so
  `Words.TXT` opens as `words.txt`. A file whose name breaks the rules (a
  space, an accent, too long) is **invisible to the app**. **About <app>**
  lists it as "ignored: rename to lower-case letters, digits, . _ -", so
  it's clear why the app can't see it.
- **Text files are UTF-8.** They draw folded to the Palm font (§5.1), so a
  book with accents still reads correctly, just without the accents.
- **The inspection runs with the shipped `data/`** (G4), and its file fuzz
  also feeds truncated and damaged copies of those files to the app. It
  reports the total size of `data/` as a NOTE.

### 5.8 An example

**A complete app, to show the shape an AI should produce:**

```c
/* dice -- roll 1..3 dice; tap to roll. */
#include "palm_app.h"

PA_APP("com.example.dice", "Dice", 1, 0, PA_CAP_NONE);

#define STATE_VER 1
static struct {
    uint8_t n;          /* dice in play, 1..3 */
    uint8_t face[3];    /* 1..6 */
    uint32_t rolls;
} S;

static void draw(void){
    pa_clear(PA_PAPER);
    for(int i = 0; i < S.n; i++){
        int16_t x = (int16_t)(20 + i * 72);
        pa_rect(x, 50, 56, 56, PA_INK);
        char buf[4];
        pa_fmt_int(buf, sizeof buf, S.face[i]);
        pa_text((int16_t)(x + 24), 72, PA_FONT_BOLD, buf, sizeof buf, PA_INK);
    }
    char rolls[16];
    pa_fmt_int(rolls, sizeof rolls, (int32_t)S.rolls);
    pa_status(1, rolls);
}

static void roll(void){
    for(int i = 0; i < S.n; i++) S.face[i] = (uint8_t)(1 + pa_random() % 6);
    S.rolls++;
}

static void on_event(const pa_event *e){
    static const char *const BTN[] = { "1", "2", "3" };
    switch(e->type){
    case PA_EV_OPEN:
        if(pa_state_load(&S, sizeof S, STATE_VER) != (int32_t)sizeof S){ S.n = 2; roll(); }
        pa_buttons(BTN, 3);
        draw();
        break;
    case PA_EV_PEN_UP:  roll(); draw(); break;
    case PA_EV_BUTTON:  S.n = (uint8_t)(e->id + 1); roll(); draw(); break;
    case PA_EV_CLOSE:   pa_state_save(&S, sizeof S, STATE_VER); break;
    default: break;
    }
}

PA_MAIN(on_event);
```

`PA_APP` puts the descriptor in a `.pa_desc` section, which `mkpack` reads to
fill in the header, so the name and version are written in one place only.
`PA_MAIN` defines `pa_entry`, saves the table pointer used by the inline
wrappers, and returns the descriptor. The title bar already reads "Dice"
(from the descriptor), and the canvas is redrawn after the event returns.

---

## 6) Palm C: the language profile

"AI-friendly" here means two things: a **small C dialect that rules out the
bugs AIs make most often in C**, and **documentation and tooling that an AI
reads and runs without human help.**

### 6.1 The rules (each one is checked by the inspection, not left to the author)

| Rule | Why | Enforced by |
|---|---|---|
| C11. Includes limited to `palm_app.h`, `stdint.h`, `stdbool.h`, `stddef.h` and the app's own headers | No libc surface, which means no `printf`/`strcpy` class of overruns | `packlint` source pass (G0); link fails on any other import (G6) |
| All state in **one `static` struct** (plus `const` tables) | Size is known at build time, state is saveable, nothing dangles | Convention and lint: no non-const globals except the state struct |
| **No `malloc`/`free`**, and at most one `pa_arena()` call | Rules out leaks, use-after-free and double free | Symbol lint; the arena is freed by the firmware |
| **No recursion**, and a **≤ 2 KB** worst-case stack | Runs on the main task's stack | `-fstack-usage -fcallgraph-info=su` → `stackcheck.py` rejects cycles and sums the deepest path (G2) |
| **No VLAs, no `alloca`** | Keeps the stack bound provable | `-Werror=vla`, symbol lint |
| Every buffer passes its size; strings go through `pa_strl*`/`pa_fmt_*` | Makes off-by-one overruns show up under ASan and bounded by construction | API shape; `-Werror=stringop-overflow`; ASan (G4) |
| No function pointers stored in the state struct | A corrupted save file can't turn into a jump | Lint on struct member types |
| `float` allowed, `double` not (`-fsingle-precision-constant -Werror=double-promotion`, and no soft-float helpers at link) | The ESP32 FPU is single precision; `double` is slow software emulation and needs imports | G1 flags; G6 symbol allow-list |
| No `volatile`, inline asm, or integer-to-pointer casts | No hardware access or firmware poking outside the API | Lint (G0) plus the instruction scan (G6) |
| Event handlers return in **≤ 50 ms** (the soft budget); long work is split across `TICK`s | Keeps the UI and the task watchdog alive | Instruction count per event in QEMU (G7); a device-side watermark (§8) |
| Pure logic lives in `logic.c`/`logic.h` with **no `pa_*` calls**, plus a `test_logic.c` | The same pattern as `sudoku.c`/`sim/tests/sudoku_test.c`: the hard part is unit-testable on a desktop | Required files, checked by `packlint`; the test must pass (G3) |

### 6.2 What an AI author gets

In `apps/` (and, after the split, the `cyd-palm-apps` template):

- **`AGENTS.md`** (with `CLAUDE.md` pointing to it): the rules above as a
  checklist, the one command to run, how to read its output, the memory and
  time budgets in plain numbers, and "do not" items (no `malloc`, no LVGL, no
  recursion, no string building without sizes).
- **`sdk/docs/API.md`, generated from `palm_app.h`**, where every function
  has a one-line contract saying what it clips, what it truncates, and what it
  returns on error (the §5.3 table, in the header's own words). The header is
  the authority, so the docs can't drift.
- **Five example apps, from small to large**, each inspected READY:
  `hello` (text and a button), `dice` (above), `counter` (state and a menu),
  `reader` (streams a text file from its folder a page at a time), and
  `sudoku` (a port of the built-in game, the reference for canvas grids,
  Graffiti digits and a play clock).
- **`make new APP=<id>`** scaffolds a folder that already inspects READY:
  `app.c`, `logic.c`/`logic.h`, `test_logic.c`, `tour.txt`, `app.toml`
  (description and waivers), a placeholder `icon.png` and an empty `data/`.
- **`sdk/inspect.sh apps/<id> --quick`** runs G0–G5 in about a minute and
  prints **one line per finding**, in compiler format
  (`file:line: G#: message`), for the edit loop. The **full inspection**
  (§7) is the last step: the AI runs it and hands you the report with the
  pack. `AGENTS.md` says an app isn't done until the full report says READY.
- **`make shots APP=<id>`** renders the app's tour to PNGs, so an AI can look
  at what it built.
- **A chat-only path:** push only `app.c` (and optional `logic.*`) to a
  branch. CI runs the full inspection and attaches the report, the
  screenshots and the pack to the run.

---

## 7) The inspection process

There is no signing and no on-device switch. The check is a **manual
inspection**: before copying a pack to the card, you (or the AI that wrote
it) run one command. It puts the app through every check below and prints a
**plain-text report** saying whether it's ready and, if not, exactly what is
wrong, where, and how to fix it. The device doesn't check whether a pack was
inspected. Loading it is your call, made with the report in front of you.

### 7.1 Running it

```
sdk/inspect.sh apps/dice              # full inspection: source + pack, stages G0–G7
sdk/inspect.sh apps/dice --quick      # G0–G5 only, about a minute: the edit loop
sdk/inspect.sh dice.pack              # a pack without its source: G6–G7 only
```

- `inspect.sh` runs inside the SDK's container image, which holds ESP-IDF
  5.5, clang, cppcheck and Espressif QEMU. The only thing to install is
  Docker; the same image runs in CI.
- The report goes to the terminal and to `inspect-report.txt`. The pack and
  a copy of the app's `data/` go to `out/<app-id>/`, the folder you copy to
  `/sdcard/apps/` (§5.7).
- **Exit code:** 0 = READY, 1 = NOT READY, 2 = INCOMPLETE. That lets an AI
  author, or a CI job on `apps/` pull requests, act on the result without
  parsing the text.
- `--quick` prints findings one per line in compiler format
  (`app.c:41: G2: …`), so an editor or tool loop can jump to them. The full
  report comes from the full run.

### 7.2 The stages

| Stage | What it runs | What it catches | Error when |
|---|---|---|---|
| **G0 Rules** | `packlint --source`: includes, banned identifiers, globals, required files, `app.toml`, the icon (24×22, black and white), `data/` file names | Code outside the Palm C profile (§6.1) | Any rule broken |
| **G1 Compile, twice** | Host `clang -m32` (32-bit, like the device) **and** Xtensa `gcc` (the IDF 5.5 toolchain) with `-std=c11 -Wall -Wextra -Wconversion -Wshadow -Wvla -Wformat=2 -Wcast-align -Wstrict-prototypes` | Truncation, sign mix-ups, shadowed state, misalignment | Any warning on either compiler |
| **G2 Static analysis + stack bound** | `gcc -fanalyzer`, `cppcheck --enable=warning,portability`, `clang-tidy` (`bugprone-*`, `cert-*`, `clang-analyzer-*`); `stackcheck.py` over `.su` and `.ci` files | Null dereferences, out-of-bounds indexes, uninitialised reads; recursion; stack over 2 KB | Any finding not waived in `app.toml` with a reason (a waived one is a warning); any call cycle; stack over budget |
| **G3 Logic tests** | `test_logic.c` with ASan + UBSan, `-m32` | Wrong rules and edge cases, before the UI is involved | Any failed check or sanitizer report |
| **G4 Simulator run** | The real firmware UI (`sim/`, 32-bit, LVGL pool and heap cap at device values), with the app compiled in through the same `palm_api.c`, under **ASan + UBSan** (including `-fsanitize=alignment`). Runs: (a) the app's `tour.txt`; (b) **a monkey run**, 20,000 random pen, button, menu and character events across 8 fixed seeds, with open/close cycles; (c) **a file fuzz**, where truncated, oversized and bit-flipped `state.bin` and data files are fed to `OPEN` | Out-of-bounds access on app state, overflow UB, unaligned access (a hard fault on LX6), crashes on unexpected input order, trusting bytes on disk, pointers passed to the API from outside the app's memory | Any sanitizer report, crash or hang (10 s per event); **any `PA_E_ARG` from the pointer check (§8.2)** |
| **G5 Resources** (same runs) | Heap and LVGL pool before open and after close; arena high-water mark; bytes written; timer rate | Leaks through the API, runaway writes, timer abuse | Heap or pool not back to baseline; any limit in §5.4 exceeded. **Warning** above 80 % of any limit |
| **G6 Binary scan** | Xtensa build → `mkpack` → `packlint --elf`: sections limited to `.text .literal .rodata .data .bss .pa_desc`; **no undefined symbols beyond the allowed helpers**; sizes within budget; **an instruction scan** for privileged and special-register instructions (`wsr`, `xsr`, `rsr` other than `ccount`, `wer`, `rer`, `rsil`, `waiti`, …); literal-pool constants pointing into peripheral (`0x3FF0_0000`–`0x3FF7_FFFF`, `0x6000_0000`+), ROM, flash-mapped or firmware code ranges | Hardware access and firmware calls that bypass the API, whether by accident or by an AI "helpfully" optimising | Any disallowed section, symbol, instruction or address constant |
| **G7 Emulator on the real loader** | A devtools firmware build (`CONFIG_CYD_DEVTOOLS`) in Espressif QEMU. The pack sits in a test-only FAT partition in flash, since QEMU has no SD card. The tour is replayed through the devtools input hook (a pack-only inspection uses a generic monkey tour). Records heap before and after, IRAM use, the stack high-water mark, and `ccount` cycles per event | Relocation and loader bugs, IRAM placement faults (byte access to `.text`), real Xtensa behaviour, slow events | A panic or watchdog reset; heap not restored; stack over 2 KB; any event over 50 ms at 240 MHz. **Warning** above 20 ms |

**Why both G4 and G7 are needed.** The sanitizers only work on the host.
They catch memory bugs in the app's own code, which is where AI-written C
goes wrong. QEMU exercises what the host can't: the loader, relocations,
IRAM rules and real Xtensa timing.

### 7.3 Severities and verdicts

| Severity | Meaning |
|---|---|
| **ERROR** | Would crash the app, corrupt memory or saved data, break a rule the device relies on, or exceed a limit the loader or the guards in §8 enforce. |
| **WARNING** | Within the limits but close to one (over 80 %), a static-analysis finding waived with a reason, an event over 20 ms, or a tour that never reaches one of the app's buttons or menu items. |
| **NOTE** | Information only, such as the measurements. |

| Verdict | When | Exit |
|---|---|---|
| **READY** | Every stage ran; no errors or warnings. | 0 |
| **READY, WITH WARNINGS** | Every stage ran; no errors. Read the warnings. | 0 |
| **NOT READY** | At least one error. Don't copy the pack to the card. | 1 |
| **INCOMPLETE** | A stage couldn't run: no source, a tool missing, QEMU unavailable. The report names each stage and the reason. Treat as not ready. | 2 |

### 7.4 What a report looks like

```
PALM APP INSPECTION REPORT
==========================
App        com.example.dice  "Dice"  version 1.0   API 1.0
Pack       apps/dice/dice.pack   18,432 bytes
SHA-256    3f9a0c1e7b2d…c21e      (About Dice on the device shows 3f9a0c1e7b2d)
Inspected  2026-10-02 14:31 UTC, SDK sdk-v1.0 (a1b2c3d), full run

VERDICT: NOT READY -- 2 errors, 1 warning. Do not copy this pack to the card.

STAGES
  G0 Rules                  PASS
  G1 Compile (host, ESP32)  PASS
  G2 Static analysis        FAIL   1 error
  G3 Logic tests            PASS   14 checks
  G4 Simulator run          FAIL   1 error
  G5 Resources              WARN   1 warning
  G6 Binary scan            PASS
  G7 Emulator (QEMU)        PASS   worst event 3.1 ms, stack 1,204 B

ERRORS
  E1  G2  app.c:41  Out-of-bounds write
      S.face[i] is written with i up to 3; face has 3 elements (0..2).
      Why it matters: it overwrites the next field of the state struct,
      which is then saved to the card.
      Fix: keep S.n <= 3 where it is set from the button id (app.c:52),
      or size face[] to the largest n.

  E2  G4  app.c:41  AddressSanitizer: global-buffer-overflow, WRITE of size 1
      Monkey run, seed 5, event 1,882 (BUTTON id 3). Same cause as E1.
      Reproduce: sdk/inspect.sh apps/dice --replay monkey:5:1882

WARNINGS
  W1  G5  Arena use 23.1 KB of 24 KB (96 %).
      Fix: nothing needed now; a bigger board size would not fit.

NOT CHECKED
  (nothing -- every stage ran)

MEASUREMENTS
  code 3,140 B / 24 KB   data 212 B / 16 KB   arena 23.1 KB / 24 KB
  stack 1,204 B / 2 KB   worst event 3.1 ms / 50 ms
  files written 0 B / 256 KB   heap after close = heap before (0 B leaked)
```

Every finding has the same four parts: **where** (stage, file and line),
**what** (one line), **why it matters on this device**, and **how to fix it**,
or how to reproduce it when the cause isn't obvious. That's what makes the
report usable by a person reading it and by an AI told to "fix what the
report says". The SHA-256 lets you confirm that the pack on the card is the
one that was inspected: **About <app>** on the device shows the first 12
characters.

**What the inspection can't prove:** that there's no bug on an input path the
tour, monkey and fuzz runs never reached, or that a *deliberately* malicious
pack is harmless (§9). The report states both on every run, under NOT
CHECKED.

---

## 8) Guards on the device

### 8.1 Around each app

These are cheap checks that run at load time and around each call. They stop
a bug from turning into a boot loop or a corrupted card.

- **Crash flag and quarantine.** Before each `on_event`, `apphost` writes the
  app's id hash to an `RTC_NOINIT` word, and clears it after the call
  returns. At boot, a set flag plus a panic or watchdog reset reason (the
  same `esp_reset_reason()` check `clock.c` already makes) puts that app in
  **quarantine**: `/sdcard/apps/quarantine.txt`, and its tile reads "Stopped
  after a crash — tap for details". One line goes to `/sdcard/apps/crash.log`
  with the app id, version, reset reason and the event type that was
  running. The device then boots to the launcher, not back into the app.
  Replacing the pack (a new SHA-256) lifts the quarantine.
- **Guard words** (16 bytes of a known pattern) on both sides of the app's
  data block, checked after **every** `on_event`. If one has changed, the
  app is stopped, **without saving its state** (the state may be corrupt),
  and quarantined.
- **Stack watermark.** The main task's high-water mark is read after each
  call. If an app pushes it under 1 KB spare, that's logged; under 512 B, the
  app is quarantined. The FreeRTOS stack canary check stays on as the last line.
- **Time watermark.** Each call is timed with `esp_timer`. Over 100 ms is
  logged; three calls over 1 s quarantine the app. The task watchdog is still
  what catches a true hang.
- **Resource balance on close.** Heap, pool and open file handles must return
  to their pre-open values. A mismatch is logged as a *firmware* bug, because
  apps can't allocate outside the API.
- **No apps during HotSync or a Coach seal.** `apphost` refuses to open an
  app while a sync is running, and closes the current app (saving it) before
  a sync starts or a Coach seal begins. (A seal already makes games
  unreachable; check in Phase 3 whether HotSync does the same, or add it.)

### 8.2 Protections built into the API and firmware (instead of encryption)

Flash encryption is ruled out (§11), so the passwords stay readable in flash
by any code that knows where to look. What the firmware *can* do is make sure
there is nothing to find by accident, and make the API useless as a tool for
finding it. These are listed in the order they'd be built.

1. **No passwords in resident RAM.** Today `g_cfg` holds every Wi-Fi password
   and the iCloud password for as long as the device is on, and `hotsync.c`
   copies them into stack buffers (`wifi_config_t wc`, the DAV `d.pass`) that
   are never wiped. Change it so that:
   - `Config` keeps only a "has password" flag for each secret;
   - HotSync, Discover and the Wi-Fi scan read each password with
     `secret_get()` into a buffer that lives only as long as the connection
     needs it, then wipe it with `mbedtls_platform_zeroize()`;
   - the Settings password fields write straight to `secretstore`, show
     `********`, and never read the password back into a form;
   - the Wi-Fi driver uses `WIFI_STORAGE_RAM`, so it keeps no second copy
     in NVS (`nvs.net80211`).

   This change is worth making even if apps never ship, and it's
   checkable: a simulator gate searches the whole heap and static memory for
   a test password after a sync and after closing Settings.
2. **Heap scrub before an app opens.** `apphost` zeroes the free heap before
   loading: it allocates every free block, zeroes it and frees it again. That
   way stale buffers from the last sync (HTTP auth headers, TLS records,
   Wi-Fi driver state) aren't lying in memory next to the app. Apps can't
   open during a sync, so nothing else is allocating at the time. Phase 0
   measures the cost; it should be about 1 ms for ~100 KB.
3. **Zeroed app memory.** The app's data block and arena are zeroed when
   allocated, and zeroed again when freed, so the next app can't read the
   previous one's data either.
4. **Pointer checks on every API argument.** Each pointer an app passes
   (strings, buffers, bitmaps, label arrays and the strings they point to)
   must lie **entirely inside the app's own memory**: its data block, its
   arena, its `.rodata`, or the main task's stack. If not, the call returns
   `PA_E_ARG`, does nothing, is logged, and the app is quarantined. This
   stops the API being used as a "confused deputy" (for example
   `pa_file_write(h, <address of a firmware buffer>, 4096)` copying firmware
   memory to the card, or `pa_text` drawing it on screen). It costs a few
   comparisons per call. In the simulator the same check makes G4 fail, so a
   bug like this is found before release.
5. **No firmware pointers are ever handed out.** Files are small integer
   handles with a generation count (so a stale handle is rejected, not
   reused). There is no framebuffer pointer: the canvas is reached only
   through drawing calls. Events arrive as a copy. An app therefore has no
   *legitimate* pointer to anything outside its own memory, which is also
   what makes the G6 address-constant scan meaningful.
6. **Files limited to the app's own folder.** File names are checked
   against `[a-z0-9._-]{1,31}`, with `data/` as the only allowed prefix (no
   other `/`, no `..`), and the firmware builds the full path itself. There's
   no call that can reach `config.ini`, the Palm databases, another app's
   folder or `/sdcard/apps/` itself. `data/` is read-only, the app's own
   files have a 256 KB quota, and `state.bin` and `app.pack` can't be opened
   directly (§5.7).
7. **No way to fake the system UI.** The title bar always shows the pack's
   display name. `pa_alert`, `pa_confirm`, `pa_pick` and `pa_prompt` boxes
   carry the app's name in their frame. `pa_prompt` always shows what is
   typed and has no masked mode, so it never looks like a password field.
   The Graffiti strip, when an app turns it on, shows the app's name. So an app can't pass itself off
   as Settings asking for the iCloud password.
8. **Capabilities in the table itself.** A slot the pack's header didn't ask
   for (in v1, only the file calls) points at a stub that returns
   `PA_E_DENIED`, so the app never gets the real function's address. The
   More folder's "About <app>" lists the capabilities the app asked for.
9. **Preferences are an allow-list.** `pa_pref` and `pa_pref_str` answer
   only the keys in §5.3 (the 12/24-hour setting, the week start, low
   battery, the owner's name). No key reaches passwords, accounts, Wi-Fi
   networks or location.
10. **Rate and size limits** on everything that reaches the outside world:
   the log (10 lines a second), the timer (≥ 100 ms), file writes (the quota),
   and state (4 KB).

**What this does and doesn't achieve.** Together these mean an app's
*mistakes* can't leak a password: there is none in RAM to overrun into, and
the API refuses to read from memory the app doesn't own. They don't stop a
*deliberately* malicious pack from calling the flash-read routines at a
computed address. Against that, the only line is the inspection's binary
scan (G6), which looks for exactly those address constants and special
instructions. It runs only if you run it, and a determined author can
compute an address at run time rather than store it.

---

## 9) Threat model

| Threat | In scope? | What covers it |
|---|---|---|
| AI-written app with a memory bug (overrun, off-by-one, uninitialised value) | **Yes, the main case** | Palm C rules, G1–G4 sanitizers and fuzzing, G7, guard words |
| App that hangs or is slow | Yes | G7 cycle budget; time watermark; watchdog; quarantine |
| App that fills the SD card or damages its own state | Yes | Write quota; `safefile`; `pa_state_*` CRC and version |
| App that crashes at boot and leaves the device in a loop | Yes | Crash flag → quarantine → boot to launcher |
| Firmware update that breaks an installed app | Yes | API versioning, ABI freeze file, app compatibility gate |
| Corrupted pack on the card | Yes | SHA-256 checked before loading |
| Pack on the card isn't the one that was inspected | Yes, by the user | The report's SHA-256 against **About <app>** (§7.4) |
| Buggy app leaking a password by accident (reading past its buffer, or handing the API a wild pointer) | Yes | Passwords out of resident RAM, the heap scrubbed before open, zeroed app memory, pointer checks on every API argument (§8.2) |
| App tricking the user into typing a password | Yes | The firmware-owned title bar; no password-field widget in the API (§8.2) |
| **Malicious pack reading the NVS passwords** | **No: mitigated, not prevented** | The G6 instruction and address scan (if the pack is inspected) and §8.2 make it hard. The device loads any well-formed pack. A determined author can still compute flash-read addresses at run time. **The plain ESP32 can't isolate an app from the firmware, and flash encryption has been ruled out (§11).** |

**The rule for users:** put a pack on the card only if you or your own AI
wrote it **and** its full inspection report says READY (or READY, WITH
WARNINGS, once you've read the warnings), and the SHA-256 in **About <app>**
matches the report.

---

## 10) Phases and exit criteria

Each phase is its own PR (or series of PRs) through the normal branch and CI
flow. The firmware stays shippable at every step: until Phase 3 lands, the
More folder shows only the built-in games.

### Phase 0 — Loader spike (measure before building)

Load a hello-world pack in QEMU and measure. The code is throwaway; the
numbers are what we keep.
- Build `elf_loader` for plain ESP32 with this `sdkconfig`. Load a
  hard-coded pack from a flash partition and call its entry point.
- **Exit criteria, all measured:**
  - The loader's flash cost is **≤ 40 KB** (if not, prototype the custom
    loader).
  - `.text` and literals land in IRAM, and `.rodata`, `.data` and `.bss` in
    DRAM. A string in `.rodata` is read correctly (no byte access to IRAM).
  - The free heap after load equals the heap before minus the data block
    (IRAM doesn't touch the data heap, confirming the earlier 16 KB result).
  - The data heap and IRAM are fully restored after unload across 50
    load/unload cycles.
  - The time to hash and load a 24 KB pack from flash is measured (from SD
    is estimated).
  - The main-task stack high-water mark during an app call is measured, which
    confirms or revises the 2 KB app budget.

### Phase 1 — Groundwork (can run alongside Phase 0)

- **Versions:** take the firmware version from `git describe`
  (`PROJECT_VER`) and show it, with the API range, in Settings ▸ About. Make
  CI's `package_firmware.py` use the same string.
- **SDK skeleton:** `sdk/` with a first `palm_app.h` (marked unstable, API
  `0.x`), the ABI freeze script, and `abicheck` in CI.
- **Licences:** `sdk/LICENSE` and `apps/LICENSE` (MIT), the additional
  permission for apps in `NOTICE` (§11), and SPDX headers in the new files.
- **Secrets out of resident RAM** (§8.2, first item). This is firmware work
  that is worth doing whether or not apps ship, so it goes first; it has its
  own simulator and device checks.
- Rename the **Games** folder to **More**. The same five tiles and the same
  code; it's the slot packs will join.

### Phase 2 — SDK and host harness

- `palm_api.c`, shared by the firmware and the simulator. Implement v1 (§5)
  on top of the existing game canvas, button strip, Graffiti strip, `safefile`
  and data layer.
- A simulator "app host" mode: `make -C sim app APP=path/to/app`, which
  compiles the app into the simulator and opens it from More.
- Tools: `mkpack.py`, `packlint.py` (source and ELF passes), `stackcheck.py`,
  the monkey and fuzz drivers, and **`inspect.sh` with its report writer**
  (§7.3–7.4). G7 shows as NOT CHECKED until Phase 3.
- Examples in `apps/`: `hello`, `dice`, `counter`.
- **A set of known-bad apps** in `sdk/tests/bad/` (an overrun, an off-by-one
  string, a use of `double`, recursion, an infinite loop, an unaligned read,
  a wild pointer passed to `pa_file_write`, a peripheral address constant).
  Each one must make the inspection report the right error, at the right
  file and line. This tests the inspector itself.
- **Exit:** the three examples come back READY from G0–G6; every known-bad
  app comes back NOT READY with the expected finding; the Sudoku port
  compiles against the API (it doesn't have to pass yet).

### Phase 3 — Device app host

- `apphost.c` and `packfmt.c`: check, load, run, unload, the guards and
  protections from §8, quarantine, the crash log, and the SHA-256 in
  **About <app>**.
- The More folder lists `/sdcard/apps/*/app.pack` tiles from their headers.
- G7 (QEMU on the real loader) goes into the inspection.
- **Exit:** the three examples pass G7. A deliberately broken test pack (an
  overrun, an infinite loop, a null dereference) is **quarantined, and the
  device boots to the launcher** in QEMU. A pack with one byte changed
  (SHA-256 mismatch) is refused. Heap after 50 open/close cycles is equal to heap at boot. A test
  pack that passes a firmware address to `pa_file_write` gets `PA_E_ARG` and
  is quarantined, and no file is written.

### Phase 4 — Prove the API, then freeze it

- Add `reader` and the `sudoku` port to `apps/`, plus `AGENTS.md`,
  `CLAUDE.md`, `make new` and `make shots`.
- **Test the docs on AI authors:** a fresh Claude Code session, given only
  `apps/`, `sdk/` and a one-paragraph app idea, produces an app that passes
  a full inspection with a READY verdict **without human edits**. Try three different ideas. Any failure
  is a gap in the docs or SDK, and gets fixed there, not in the app.
- Review §5.3 with what the five examples and three AI-written apps actually
  called. Drop unused slots, add anything that was missing, then freeze
  **`sdk-v1.0`**: API `1.0` and `abi/pa_api_v1.txt`. Add the app
  compatibility gate to `ci.yml`.
- **Exit:** `sdk-v1.0` is tagged, and every app in `apps/` has a release
  built against it, with a READY report attached.

### Phase 5 — On glass, then split the repo

- Bench checks on real hardware: install from the card, the SD-mounted heap
  figures (replacing the estimate), tile draw time with 10 packs, the
  quarantine path after a real panic, and the Sudoku port's speed next to
  the built-in game. Record them in `BUILD_PROGRESS.md`.
- **Split:** move `apps/` to `cyd-palm-apps` with its history, add
  `palm-sdk.lock` pinned to `sdk-v1.0`, and point the compatibility gate at the new repo. `apps/` leaves this repo.
- **Exit:** a pack released from `cyd-palm-apps` installs and runs on the
  bench device, and a firmware PR here runs that repo's apps in its
  compatibility gate.

### Later (not planned yet)

- `PA_CAP_PIM_READ` (read-only Date Book, To Do and Memo access, with a
  first-launch prompt).
- Downloading packs during HotSync from the apps repo's releases.
- Moving a built-in game out to a pack to shrink the image.

---

## 11) Decisions (made 2026-09-27)

1. **Repo split: wait until the API is frozen.** Apps live in `apps/` in this
   repo, with their own licence, workflow and tags, until `sdk-v1.0`. Then
   they move to `cyd-palm-apps` (§4.1, Phase 5).
2. **Licence: MIT** for `sdk/` and `apps/`. The firmware stays GPLv3. So that
   an MIT app loaded into it isn't treated as a derivative work, `NOTICE`
   gets an **additional permission under GPLv3 §7**: a program that talks to
   the firmware only through the `pa_api` table in `palm_app.h` may be
   distributed under any licence. This permission can only come from the
   copyright holder, and only for the code the project owns. The
   PumpkinOS-derived fonts and icons are data that apps never link against
   or receive. Have the exact wording checked before the first `app-*`
   release.
3. **No signing, no Developer Mode.** *(Revised 2026-09-27; the first
   answer was "signing from the first pack".)* Apps are checked by a
   **manual inspection** that prints a text report of any issues (§7). The
   device loads any well-formed pack. It checks the pack's structure,
   SHA-256 and API version, and the guards in §8 still run, but it doesn't
   know or care whether the pack was inspected.
4. **API list:** 44 slots in §5.3, with budgets in §5.4 and exclusions in
   §5.5. The 2026-09-27 review:
   - **Text** is UTF-8 in and drawn in the Palm font, with accents and
     ligatures folded to plain ASCII (`é → e`).
   - **Added:** `pa_text_box`, `PA_GRAY`, `pa_pick`, `pa_fail`,
     `pa_pref`/`pa_pref_str`, `pa_prompt`/`pa_prompt_text`, `pa_scroll`,
     the tile icon (§5.6) and the folder and data-file rules (§5.7).
   - **Considered and not added:** keeping the screen awake, and a backlight
     pulse.

   The list is reviewed again against real use before the freeze (Phase 4).
5. **No flash or NVS encryption.** Protection comes from the API and the
   firmware instead (§8.2). What that can't prevent is stated in §9.
