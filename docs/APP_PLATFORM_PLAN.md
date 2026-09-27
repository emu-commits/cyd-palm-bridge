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
7. [The validation pipeline](#7-the-validation-pipeline)
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
- **Checked before it runs.** Every app passes a fixed series of memory-safety
  and behaviour checks in CI before it is signed. By default the device loads
  only signed apps.

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
| LVGL runs on | the **main task** (`lvgl_port.c`, 16 KB stack, about 3.5 KB spare at the `ui_init` peak) | Apps run inside LVGL callbacks on this stack, with no task of their own. Each app gets a **2 KB stack budget**, proven statically (§7, gate G2). |
| Secrets | Wi-Fi and iCloud passwords in NVS, **not encrypted** (`secretstore.h`), **and copied into RAM for as long as the device is on** (`g_cfg` in `appcfg.c`, then into stack buffers in `hotsync.c` that are never wiped) | Native code could read them. Signing (§4.4) and the API-side protections (§8.2) cover this; the first of those protections takes the passwords out of resident RAM. |
| ESP32 IRAM access | 32-bit aligned access only | Only `.text` (and literals) can go in IRAM. `.rodata`, `.data` and `.bss` must go in DRAM. Byte loads from IRAM fault. |
| Xtensa LX6 | Unaligned 32-bit loads raise `LoadStoreAlignment` | UBSan's alignment check runs in the host gates (G4). |

---

## 3) Architecture

```
 SD card                             Firmware (factory partition)
 /sdcard/apps/                       ┌──────────────────────────────────────────┐
   dice/                             │ ui.c ── "More" folder (was "Games")      │
     app.pack  ──── read, verify ──▶ │   built-ins: Mines Wordie Sudoku Zip …   │
     icon.bin                        │   packs:     one tile per installed app   │
     data/…   ◀── pa_file_* API ───  │                                          │
     state.bin ◀─ pa_state_* API ──  │ apphost.c  verify → load → run → unload  │
   crash.log                         │   ├─ packfmt.c   header + signature      │
                                     │   ├─ loader      ELF relocation (IRAM)   │
                                     │   └─ guards      canaries, crash flag,   │
                                     │                  time/stack watermarks   │
                                     │ palm_api.c  the pa_api table (v1.x)      │
                                     │   (compiled into the simulator too)      │
                                     └──────────────────────────────────────────┘
```

**Life of an app, on the device:**

1. The **More** folder lists the built-in games plus each `/sdcard/apps/*/app.pack`.
   To draw a tile it reads only the pack header (name, icon, API version).
   Nothing is loaded yet.
2. On tap, `apphost` **verifies** the pack: magic, format version, API
   compatibility, size limits, SHA-256, then the signature (§4.4). It also
   refuses an app that is in quarantine (§8).
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
  only import is the `pa_api` pointer it is given. The linter in gate G6 can
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
| `sdk/` | `palm_app.h`, linker script, build rules, `mkpack`, `packlint`, `stackcheck`, the ABI freeze file, the public signing key(s), `docs/` | **MIT** (§11) | `ci.yml` (ABI check) plus `apps.yml` | `sdk-vA.B` tags once frozen |
| `apps/` | One folder per app, `AGENTS.md`/`CLAUDE.md` for AI authors, the examples | **MIT** (§11) | **`apps.yml`**, triggered only by changes under `apps/**` or `sdk/**`, calling the reusable `validate-app.yml` | **`app-<id>-vX.Y`** tags → a release carrying the signed `.pack` |

Two rules keep the later split cheap:
- **Nothing in `apps/` includes anything outside `sdk/`.** `packlint`
  enforces it (G0), so an app never picks up a firmware header by accident.
- **`apps.yml` builds the SDK from `sdk/` and the simulator from the same
  commit**, exactly as the external repo will do from a pinned tag.

**At `sdk-v1.0`** (Phase 5), `apps/` moves to a new template repo,
**`cyd-palm-apps`**, with its history (`git filter-repo --subdirectory-filter
apps`). It gets a `palm-sdk.lock` file naming the `sdk-vA.B` tag, and its CI
checks out this repo at that tag for the SDK, simulator and tools. The signing
key moves with it (§4.4). Moving an app to a newer SDK then becomes a
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
128     …     ELF (relocatable, Xtensa)
end-72  72    signature: ECDSA P-256 over bytes [0, end-72), DER, zero-padded
```

- **ECDSA P-256 over mbedTLS**, which is already linked for TLS, so verifying
  costs close to nothing in flash. (mbedTLS doesn't provide Ed25519.)
- **Signed from the first pack** (§11). The signing path exists before the
  loader does: the first pack CI ever builds is signed, and the device has
  never had a mode where unsigned packs load by default.
- **The public key is compiled into the firmware** (`sdk/keys/release.pub`).
  There are **two slots, current and next**, so the key can be rotated
  without breaking installed packs: a firmware release adds the next key,
  and packs are re-signed with it over time.
- **The private key is a GitHub Actions secret in an environment called
  `app-signing`**, limited to `app-*` tags and requiring approval. Today it
  lives in this repo; at the split it moves to `cyd-palm-apps`. Only the
  release job (G8) can read it, and only after every gate in §7 has passed on
  the same commit. It never goes on a laptop.
- **Developer Mode** (Settings ▸ About, behind a confirmation) also allows
  unsigned packs, for trying a pack from `make pack` before it is released.
  It's off by default, those packs are marked "Unverified" on their tile, and
  the setting turns itself off after 24 hours.

The pack format lets the device check the file cheaply before loading
anything. The signature means "this passed our CI gates". It does not mean
"this code is safe" (§9).

---

## 5) The app SDK

One header, `palm_app.h`, is the whole API. The **v1 list below is the
proposal for review.** I chose it by checking what the four built-in games
use today:
- a 1-bpp canvas with pen down, drag and up;
- two or three status labels;
- one or two buttons ("New", "Dig/Flag");
- the Palm font in regular and bold;
- a play clock;
- a save file.

Everything a game uses is in v1. Anything only *possibly* useful waits for
1.1, because once `sdk-v1.0` is frozen each slot has to be kept for good.

### 5.1 Conventions

- **Canvas:** 240×164 pixels, 1 bpp, with the origin at the top left.
  Colours are `PA_PAPER`, `PA_INK` and `PA_XOR`; XOR is the Palm-style
  selection highlight, so no separate `invert` call is needed.
- **Every drawing call clips to the canvas.** The firmware redraws the canvas
  after each `on_event` in which something was drawn, so there is no
  `invalidate` call for an app to forget.
- **Errors:** functions that can fail return `pa_err` (`int32_t`):
  - `PA_OK` = 0
  - `PA_E_ARG`: bad argument, including a pointer outside the app's own memory (§8.2)
  - `PA_E_DENIED`: the app lacks the capability
  - `PA_E_NOTFOUND`
  - `PA_E_NOSPACE`: over a quota
  - `PA_E_LIMIT`: too many of something
  - `PA_E_IO`
  - `PA_E_BUSY`

  They never fault.
- **Strings:** ASCII bytes. Every string argument is read up to its NUL or a
  cap stated below, whichever comes first. Bytes outside printable ASCII draw
  as `?`.
- **The title bar belongs to the firmware.** It always shows the pack's
  display name, so an app can't make itself look like Settings or a password
  prompt (§8.2). That's why there is no `pa_title()`.

### 5.2 Events

`on_event(const pa_event *e)`, with `pa_event = { uint8_t type; uint8_t id;
int16_t x, y; uint32_t ch; }`:

| Event | Fields | When |
|---|---|---|
| `PA_EV_OPEN` | — | Once, after load. Restore state and draw here. |
| `PA_EV_CLOSE` | — | Once, before unload (Home, the app calling `pa_exit`, a HotSync starting, a Coach seal). Save state here. There are no more events after it. |
| `PA_EV_PEN_DOWN` / `PEN_MOVE` / `PEN_UP` | `x`, `y` in canvas coordinates | Touches on the canvas. `PEN_MOVE` is sent at most every 20 ms. |
| `PA_EV_BUTTON` | `id` 0–3 | A tap on one of the app's buttons. |
| `PA_EV_MENU` | `id` 0–5 | One of the app's menu items. |
| `PA_EV_CHAR` | `ch`: ASCII, `'\b'`, `'\n'` | A Graffiti stroke, while the Graffiti strip is on. |
| `PA_EV_TICK` | — | The app's timer. It doesn't fire while the screen is off. |
| `PA_EV_CONFIRM` | `id` as passed to `pa_confirm`; `x` = 1 for yes, 0 for no | The answer to a confirmation. |

### 5.3 The proposed v1 table (slot order is the ABI)

| # | Call | Contract |
|---|---|---|
| | **Meta** | |
| 0 | `uint16_t pa_api_version(void)` | `(major << 8) \| minor`, as served by the firmware. |
| | **Canvas** | |
| 1 | `void pa_clear(uint8_t colour)` | Fill the whole canvas. |
| 2 | `void pa_pixel(int16_t x, int16_t y, uint8_t colour)` | |
| 3 | `void pa_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t colour)` | |
| 4 | `void pa_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t colour)` | Outline. |
| 5 | `void pa_fill(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t colour)` | Filled. With `PA_XOR` it is the selection highlight. |
| 6 | `void pa_disc(int16_t cx, int16_t cy, int16_t r, uint8_t colour)` | Filled circle, r ≤ 80. |
| 7 | `pa_err pa_blit(const uint8_t *bits, int16_t w, int16_t h, int16_t x, int16_t y, uint8_t colour)` | 1-bpp bitmap, rows padded to bytes, MSB first. Set bits are drawn in `colour`; clear bits are left alone. Reads exactly `((w+7)/8)*h` bytes, all of which must be in app memory. |
| 8 | `int16_t pa_text(int16_t x, int16_t y, uint8_t font, const char *s, uint16_t maxlen, uint8_t colour)` | `font` is `PA_FONT_STD` or `PA_FONT_BOLD` (the 11 px Palm fonts). Draws at most `maxlen` bytes on one line and returns the width drawn. |
| 9 | `int16_t pa_text_width(uint8_t font, const char *s, uint16_t maxlen)` | For centring and wrapping. |
| 10 | `uint8_t pa_font_height(uint8_t font)` | |
| | **Chrome** (drawn by the firmware in Palm style) | |
| 11 | `pa_err pa_buttons(const char *const *labels, uint8_t n)` | 0–4 buttons in the bottom strip, each label ≤ 10 bytes. `n = 0` removes them. |
| 12 | `pa_err pa_button_label(uint8_t id, const char *label)` | Relabel one button, for toggles like Dig/Flag, without rebuilding the strip. |
| 13 | `void pa_status(uint8_t slot, const char *s)` | Slot 0 is left-aligned and slot 1 right-aligned, on the line above the buttons (games put a status on the left and a time on the right). ≤ 24 bytes each. |
| 14 | `pa_err pa_menu(const char *const *labels, uint8_t n)` | 0–6 app items at the top of Menu. The firmware always adds "About <app>", which shows the pack's id, version, API version and signature state. |
| 15 | `void pa_alert(const char *s)` | Modal OK box, ≤ 160 bytes. |
| 16 | `void pa_confirm(const char *s, uint8_t id)` | Yes/No box. The answer arrives as `PA_EV_CONFIRM`. |
| 17 | `pa_err pa_graffiti(uint8_t mode)` | `PA_GRAF_OFF`, `PA_GRAF_LETTERS` or `PA_GRAF_DIGITS`. Shows the Graffiti strip; strokes arrive as `PA_EV_CHAR`. |
| | **Time** | |
| 18 | `int64_t pa_now(void)` | UNIX seconds, or 0 if the clock has never been set. |
| 19 | `pa_err pa_localtime(int64_t t, pa_tm *out)` | In the device's time zone. `pa_tm` holds year, month, day, hour, minute, second and weekday. |
| 20 | `uint32_t pa_ticks_ms(void)` | Monotonic milliseconds since boot. |
| 21 | `pa_err pa_timer(uint16_t period_ms)` | One repeating `PA_EV_TICK`. 0 stops it; the minimum is 100 ms. |
| 22 | `uint32_t pa_random(void)` | Hardware RNG. For reproducible boards, seed the header-only `pa_rng` from it. |
| | **State** | |
| 23 | `int32_t pa_state_load(void *buf, uint32_t len, uint16_t ver)` | Returns `len` on success. If the file is missing, is a different size or version, or fails its CRC, it returns an error and **leaves `buf` untouched**. |
| 24 | `pa_err pa_state_save(const void *buf, uint32_t len, uint16_t ver)` | Crash-safe write (`safefile`) to `state.bin`, ≤ 4 KB. |
| | **Files** (`PA_CAP_FILES`; the app's own folder only) | |
| 25 | `int32_t pa_file_open(const char *name, uint8_t mode)` | `PA_READ`, `PA_WRITE` (truncates) or `PA_APPEND`. Names match `[a-z0-9._-]{1,31}`; `state.bin` and `app.pack` are reserved. At most 2 open at once. Returns a handle ≥ 0. |
| 26 | `int32_t pa_file_read(int32_t h, void *buf, uint32_t len)` | Bytes read, 0 at the end of the file. |
| 27 | `int32_t pa_file_write(int32_t h, const void *buf, uint32_t len)` | Counts against the app's **256 KB** folder quota. |
| 28 | `pa_err pa_file_seek(int32_t h, uint32_t pos)` | |
| 29 | `int32_t pa_file_size(int32_t h)` | |
| 30 | `pa_err pa_file_close(int32_t h)` | The firmware closes any handles still open on unload. |
| 31 | `pa_err pa_file_remove(const char *name)` | |
| 32 | `int32_t pa_file_list(uint16_t index, char *name, uint16_t cap)` | The n-th file in the app's folder (reserved names skipped), or `PA_E_NOTFOUND` past the end. |
| | **Memory** | |
| 33 | `void *pa_arena(uint32_t bytes)` | One zeroed block per session, ≤ 24 KB, freed on unload. A second call returns NULL. |
| | **System** | |
| 34 | `void pa_log(const char *s)` | One line to the serial console (and the simulator's stdout), ≤ 80 bytes, at most 10 lines a second. |
| 35 | `void pa_exit(void)` | Ask to close. `PA_EV_CLOSE` follows after the current event returns. |

That's **36 slots**. There are no capabilities besides `PA_CAP_FILES` in v1.
The capability field in the pack header is 32 bits, so later ones (such as
`PA_CAP_PIM_READ`) have room.

**Compiler helpers the loader resolves by name** (not in the table; the G6
allow-list): `memcpy`, `memset`, `memmove`, `memcmp`, and the 64-bit integer
helpers (`__divdi3`, `__udivdi3`, `__moddi3`, `__umoddi3`, `__ashldi3`,
`__ashrdi3`, `__lshrdi3`, `__muldi3`). Single-precision `float` runs on the
ESP32's FPU and needs no helper. `double` would pull in soft-float helpers,
so it is not allowed (§6.1).

**Header-only helpers** (compiled into the app, not part of the ABI, and free
to improve between SDK versions): `pa_strlcpy`, `pa_strlcat`, `pa_fmt_int`,
`pa_fmt_time` (`m:ss` / `h:mm:ss`), `pa_min`/`pa_max`/`pa_clamp`,
`PA_COUNTOF`, `pa_rng` (seeded xorshift32, so generators are reproducible and
host-testable like `sd_new`), and `playclock.h` (unchanged from the firmware).

### 5.4 Budgets (enforced by the gates and by the loader)

| Resource | Limit |
|---|---|
| Code (`.text` and literals, in IRAM) | 24 KB |
| Data (`.rodata`, `.data` and `.bss`, in DRAM) | 16 KB. Large tables such as word lists go in the app's folder as files. |
| Arena | 24 KB, one block |
| Stack, worst case | 2 KB, no recursion |
| Time per event | 50 ms (G7), with a device watermark at 100 ms (§8) |
| State file | 4 KB |
| Folder on the SD card | 256 KB written in total |
| Open files | 2 |
| Buttons / menu items / status slots | 4 / 6 / 2 |

With an app at its maximum (16 + 24 KB of data), about **66 KB** of the
~106 KB heap stays free for the system. That's more than the 48 KB-heap
MicroPython option would have left.

### 5.5 Deferred to 1.1 or later (not in v1 on purpose)

| Deferred | Why not now |
|---|---|
| A large-digit font (the lock screen's `DASH_DIG`), circle outlines, polygons | Apps can draw them with `pa_blit` and `pa_line`. Add them once two apps have needed them. |
| A text-field widget | `PA_EV_CHAR` plus `pa_text` covers entry. A widget costs LVGL pool and fixes a look before any app has asked for one. |
| Sleep/wake events | Play clocks already work from `pa_now()`. |
| A second timer, a sub-100 ms timer | No game needs it; it costs battery. |
| Read-only PIM access (`PA_CAP_PIM_READ`) | Needs a consent prompt design, and a decision on what an app may see. |
| Per-app settings in Settings | An app's own menu covers it. |
| Sound | No hardware yet (`PRODUCT_PLAN.md` §4). |
| Networking, NVS, raw LVGL, threads | Out of scope (§1). |

### 5.6 An example

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

### 6.1 The rules (each one is enforced; none rely on the author's discipline)

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
- **Five example apps, from small to large**, each passing every gate:
  `hello` (text and a button), `dice` (above), `counter` (state and a menu),
  `reader` (streams a text file from its folder a page at a time), and
  `sudoku` (a port of the built-in game, the reference for canvas grids,
  Graffiti digits and a play clock).
- **`make new APP=<id>`** scaffolds a folder that already passes every gate.
- **`make check APP=<id>`** runs G0–G5 locally in about a minute and prints
  **one line per finding**, in compiler format (`file:line: gate: message`),
  followed by `OK` or `FAIL gate G#`. It is built to be read by a tool loop.
- **`make shots APP=<id>`** renders the app's tour to PNGs, so an AI can look
  at what it built.
- **A chat-only path:** push only `app.c` (and optional `logic.*`) to a
  branch. CI runs every gate and attaches screenshots and an unsigned test
  pack to the run. Tagging `app-<id>-vX.Y` produces the signed pack.

---

## 7) The validation pipeline

Every gate runs in CI through **one reusable workflow**,
`.github/workflows/validate-app.yml` (`workflow_call`). `apps.yml` calls it
today; after the split, `cyd-palm-apps` calls it pinned to its `sdk-vA.B`
tag. All apps get the same checks. G0–G5
also run locally with `make check`.

| Gate | What it runs | What it catches | Fails when |
|---|---|---|---|
| **G0 Policy lint** | `packlint --source`: includes, banned identifiers, globals, required files, manifest sanity | Out-of-profile code (§6.1) | Any violation |
| **G1 Strict compile, twice** | Host `clang -m32` (32-bit, like the device) **and** Xtensa `gcc` (the IDF 5.5 toolchain) with `-std=c11 -Wall -Wextra -Wconversion -Wshadow -Wvla -Wformat=2 -Wcast-align -Wstrict-prototypes -Werror` | Truncation, sign mixups, shadowed state, misalignment | Any warning on either compiler |
| **G2 Static analysis + stack bound** | `gcc -fanalyzer`, `cppcheck --enable=warning,portability`, `clang-tidy` (`bugprone-*`, `cert-*`, `clang-analyzer-*`); `stackcheck.py` over `.su` and `.ci` files | Null dereferences, out-of-bounds indexes, uninitialised reads; recursion; stack over 2 KB | Any finding not waived in `app.toml` with a reason; any call cycle; stack > budget |
| **G3 Logic tests** | `test_logic.c` built with ASan + UBSan, **`-m32`** so type sizes and struct layout match the device | Wrong game rules and edge cases, before the UI is involved | Any failed check or sanitizer report |
| **G4 Simulator under sanitizers** | The real firmware UI (`sim/`, 32-bit, LVGL pool and heap cap at device values) with the app compiled in through the same `palm_api.c`; **ASan + UBSan** (including `-fsanitize=alignment`). Runs (a) the app's scripted `tour.txt`, (b) **a monkey run**: 20,000 random pen, button, menu and char events across 8 fixed seeds, with open/close cycles, (c) **a state-file fuzz**: truncated, oversized and bit-flipped `state.bin` and data files fed to `OPEN` | Out-of-bounds reads and writes on app state, overflow UB, unaligned access (a hard fault on LX6), crashes on unexpected input order, trust in on-disk bytes, pointers passed to the API from outside the app's memory | Any sanitizer report, crash or hang (10 s per event); **any `PA_E_ARG` from the pointer check (§8.2)** |
| **G5 Resource checks** (same runs) | The simulator records heap and pool before open and after close, the arena high-water mark, file bytes written, and timer rate | Leaks through the API, runaway writes, timer abuse | Heap or pool not back to baseline; writes over quota; state over declared size |
| **G6 Pack build + binary lint** | Xtensa build → `mkpack` → `packlint --elf`: sections are only `.text .literal .rodata .data .bss .pa_desc`; **no undefined symbols except the allowed helpers**; sizes within budget; **an instruction scan** that rejects privileged and special-register instructions (`wsr`, `xsr`, `rsr` except `ccount`, `wer`, `rer`, `rsil`, `waiti`, and others) and literal-pool constants that point into peripheral (`0x3FF0_0000`–`0x3FF7_FFFF`, `0x6000_0000`+), ROM or firmware code ranges | Hardware access and firmware calls that bypass the API, whether by accident or by an AI "helpfully" optimising | Any disallowed section, symbol, instruction or address constant |
| **G7 QEMU on the real loader** | A devtools firmware build (`CONFIG_CYD_DEVTOOLS`) in Espressif QEMU. The pack is placed in a test-only FAT partition in flash (QEMU has no SD card). The app's tour is replayed through the devtools input hook. The run records heap before and after, IRAM use, the app's stack high-water mark, and `ccount` cycles per event | Relocation and loader bugs, IRAM placement faults (byte access to `.text`), real Xtensa behaviour, and events that are too slow | A panic or watchdog reset; heap not restored; stack > 2 KB; any event > 50 ms at 240 MHz |
| **G8 Sign** (release job only) | Every gate above passed on this commit → sign with the release key → attach `<id>-vX.Y.pack` to a release | — | Only runs on a protected tag |

**Why G4 and G7 are both needed.** The sanitizers work only on the host. They
catch memory bugs in the app's own code, which is where AI-written C goes
wrong. QEMU exercises the parts the host can't: the loader, relocations, IRAM
rules and real Xtensa timing. Neither one alone is enough.

**Things this pipeline can't prove, stated so nobody assumes it does:** that
the app has no bug on an input path none of the tour, monkey or fuzz runs
reached, and that a *deliberately* malicious pack is harmless (§9).

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
   against `[a-z0-9._-]{1,31}` (no `/`, no `..`), and the firmware builds the
   full path itself. There's no call that can reach `config.ini`, the Palm
   databases, another app's folder or `/sdcard/apps/` itself. There's a
   256 KB write quota, and `state.bin` and `app.pack` can't be opened
   directly.
7. **No way to fake the system UI.** The title bar always shows the pack's
   display name. `pa_alert` and `pa_confirm` boxes carry the app's name in
   their frame. The API has no password field, and the Graffiti strip, when
   an app turns it on, shows the app's name. So an app can't pass itself off
   as Settings asking for the iCloud password.
8. **Capabilities in the table itself.** A slot the pack's header didn't ask
   for (in v1, only the file calls) points at a stub that returns
   `PA_E_DENIED`, so the app never gets the real function's address. The
   More folder's "About <app>" lists the capabilities the app asked for.
9. **Rate and size limits** on everything that reaches the outside world:
   the log (10 lines a second), the timer (≥ 100 ms), file writes (the quota),
   and state (4 KB).

**What this does and doesn't achieve.** Together these mean an app's
*mistakes* can't leak a password: there is none in RAM to overrun into, and
the API refuses to read from memory the app doesn't own. They don't stop a
*deliberately* malicious pack from calling the flash-read routines at a
computed address. For that, the only protection is signing (§4.4): CI signs
only packs that passed G6, and the device loads only signed packs.

---

## 9) Threat model

| Threat | In scope? | What covers it |
|---|---|---|
| AI-written app with a memory bug (overrun, off-by-one, uninitialised value) | **Yes, the main case** | Palm C rules, G1–G4 sanitizers and fuzzing, G7, guard words |
| App that hangs or is slow | Yes | G7 cycle budget; time watermark; watchdog; quarantine |
| App that fills the SD card or damages its own state | Yes | Write quota; `safefile`; `pa_state_*` CRC and version |
| App that crashes at boot and leaves the device in a loop | Yes | Crash flag → quarantine → boot to launcher |
| Firmware update that breaks an installed app | Yes | API versioning, ABI freeze file, app compatibility gate |
| Tampered or corrupted pack on the card | Yes | SHA-256 plus signature; refused unless in Developer Mode |
| Buggy app leaking a password by accident (reading past its buffer, or handing the API a wild pointer) | Yes | Passwords out of resident RAM, the heap scrubbed before open, zeroed app memory, pointer checks on every API argument (§8.2) |
| App tricking the user into typing a password | Yes | The firmware-owned title bar; no password-field widget in the API (§8.2) |
| **Malicious pack reading the NVS passwords** | **No: mitigated, not prevented** | Signing (only CI-validated packs load by default), the G6 instruction and address scan, and §8.2 make it hard. A determined author can still compute flash-read addresses at run time. **The plain ESP32 can't isolate an app from the firmware, and flash encryption has been ruled out (§11).** |

**The rule for users:** install only packs that you or your own AI wrote, or
that come signed from the apps repo. The Developer Mode warning says the
same thing.

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
  - The time to verify an ECDSA P-256 signature is measured, as is the time
    to load a 24 KB pack from flash (from SD is estimated).
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
- **Signing keys:** generate the P-256 pair, put the public half in `sdk/keys/`,
  and create the `app-signing` environment holding the private half.
  `mkpack --sign` and `--verify` exist from here, before any loader.
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
  the monkey and fuzz drivers.
- Examples in `apps/`: `hello`, `dice`, `counter`. `make check` passes on all
  three, and `apps.yml` builds a **signed** pack of each on an `app-*` tag.
- **Exit:** all three examples pass G0–G5 locally and in CI, and the Sudoku
  port compiles against the API (it doesn't have to pass yet).

### Phase 3 — Device app host

- `apphost.c` and `packfmt.c`: verify, load, run, unload, the guards and
  protections from §8, quarantine, crash log, Developer Mode, and the
  release public keys.
- The More folder lists `/sdcard/apps/*/app.pack` tiles from their headers.
- G7 (QEMU on the real loader) goes into CI.
- **Exit:** the three examples pass G7. A deliberately broken test pack (an
  overrun, an infinite loop, a null dereference) is **quarantined, and the
  device boots to the launcher** in QEMU. A pack whose signature is wrong is
  refused. Heap after 50 open/close cycles is equal to heap at boot. A test
  pack that passes a firmware address to `pa_file_write` gets `PA_E_ARG` and
  is quarantined, and no file is written.

### Phase 4 — Prove the API, then freeze it

- Add `reader` and the `sudoku` port to `apps/`, plus `AGENTS.md`,
  `CLAUDE.md`, `make new`, `make check` and `make shots`.
- **Test the docs on AI authors:** a fresh Claude Code session, given only
  `apps/`, `sdk/` and a one-paragraph app idea, produces an app that passes
  every gate **without human edits**. Try three different ideas. Any failure
  is a gap in the docs or SDK, and gets fixed there, not in the app.
- Review §5.3 with what the five examples and three AI-written apps actually
  called. Drop unused slots, add anything that was missing, then freeze
  **`sdk-v1.0`**: API `1.0` and `abi/pa_api_v1.txt`. Add the app
  compatibility gate to `ci.yml`.
- **Exit:** `sdk-v1.0` is tagged, and every app in `apps/` has a signed
  release built against it.

### Phase 5 — On glass, then split the repo

- Bench checks on real hardware: install from the card, the SD-mounted heap
  figures (replacing the estimate), tile draw time with 10 packs, the
  quarantine path after a real panic, and the Sudoku port's speed next to
  the built-in game. Record them in `BUILD_PROGRESS.md`.
- **Split:** move `apps/` to `cyd-palm-apps` with its history, add
  `palm-sdk.lock` pinned to `sdk-v1.0`, move the `app-signing` secret there,
  and point the compatibility gate at the new repo. `apps/` leaves this repo.
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
3. **Signing from the first pack.** The keys and `mkpack --sign` come in
   Phase 1, before the loader exists (§4.4). Developer Mode is still there
   for testing, off by default, and turns itself off after 24 hours.
4. **API list:** proposed in §5.3 (36 slots) with budgets in §5.4 and
   exclusions in §5.5. **Still open for your review.** It is reviewed again
   against real use before the freeze (Phase 4).
5. **No flash or NVS encryption.** Protection comes from the API and the
   firmware instead (§8.2). What that can't prevent is stated in §9.
