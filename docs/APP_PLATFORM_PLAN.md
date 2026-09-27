# App platform plan — native C apps from the SD card

> **Status: PROPOSAL, not approved.** Nothing here is built. It follows the
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
11. [Decisions we need to make](#11-decisions-we-need-to-make)

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
  checks catch *bugs*, not malice.
- **Network access from apps.** Wi-Fi + TLS needs more than 100 KB of heap,
  which can't sit beside an app.
- **Apps building their own LVGL widget trees.** The LVGL pool has about
  11 KB free with the launcher showing. Apps draw on the shared 1-bpp canvas
  and use firmware-built controls.
- **Moving the built-in games out of the firmware.** They stay in C in the
  image. We can revisit this once the API has been stable for a while (§11).

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
| Secrets | Wi-Fi and iCloud passwords in NVS, **not encrypted** (`secretstore.h`) | Native code could read them. This is the main reason the device loads only signed apps by default (§9). |
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

### 4.1 Repositories

| Repo | Holds | Releases |
|---|---|---|
| **`cyd-palm-bridge`** (this one) | Firmware, simulator, **and the SDK** (`sdk/`: `palm_app.h`, linker script, build rules, `mkpack`, `packlint`, the reusable validation workflow). The SDK sits here because `palm_api.c` implements it and changes to it have to be reviewed together. | `vX.Y.Z` firmware tags (as today). **`sdk-vA.B`** tags mark the commit apps build against. |
| **`cyd-palm-apps`** (new, a GitHub template repo) | One folder per app, `AGENTS.md`/`CLAUDE.md` for AI authors, a `palm-sdk.lock` file naming the `sdk-vA.B` tag, and a CI workflow that calls the SDK's reusable validation workflow. | `<app-id>-vX.Y` tags → a GitHub release carrying the signed `.pack`. |

The apps repo pins an SDK tag and doesn't vendor the firmware. When CI runs,
it checks out `cyd-palm-bridge` at that tag to get the headers, the simulator
and the tools. Moving an app to a newer SDK is a one-line PR that bumps the
lock file.

**Alternative (smaller first step):** start with `apps/` in this repo, behind
its own workflow and tags, and move it out once `sdk-v1.0` is frozen. It's
cheaper for Phase 2, but it doesn't give the separation you asked for, so
it's listed under §11.

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
  `apphost.c` (and nightly), CI builds the SDK's example apps and every app in
  `cyd-palm-apps` at its released tag, then runs their tours against the new
  firmware in the simulator. This gate is what makes the split safe: a
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
72      4     capability bits (PA_CAP_FILES, PA_CAP_TIMER, … — see §5)
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
- **The public key is compiled into the firmware** (`sdk/keys/release.pub`).
  The private key is a secret in `cyd-palm-apps`, and only the release job,
  run from a protected branch or tag, can use it. That job runs only after
  every gate in §7 has passed on the same commit.
- **Developer Mode** (Settings ▸ About, behind a confirmation) also allows
  unsigned packs. They are marked "Unverified" on their tile, and the setting
  turns itself off after 24 hours. This is how a Claude session or a
  developer tries a pack from `make pack` before CI signs it.

The pack format lets the device check the file cheaply before loading
anything. The signature means "this passed our CI gates". It does not mean
"this code is safe" (§9).

---

## 5) The app SDK

One header, `palm_app.h`, is the whole API. Its **v1 surface** is only what
the four built-in games would need if they were written as packs. That is
the test: if Mines, Sudoku, Wordie and Zip can be written with it, the API
is enough for v1.

| Area | Functions (sketch) | Capability bit |
|---|---|---|
| Lifecycle | `pa_entry(api)` → `const pa_app *`; the descriptor holds `on_event`. Events: `OPEN`, `CLOSE`, `PEN_DOWN/MOVE/UP` (canvas coordinates), `BUTTON(id)`, `MENU(id)`, `CHAR(c)` from the Graffiti strip, `TICK` | — |
| Canvas (1 bpp, 240×164) | `pa_clear`, `pa_pixel`, `pa_line`, `pa_rect`, `pa_fill`, `pa_disc`, `pa_blit1(bits, w, h, x, y)`, `pa_text(x, y, font, str, maxlen)`, `pa_text_width`, `pa_invalidate` — **every call clips to the canvas** | — |
| Chrome | `pa_title(str)`, `pa_buttons(labels[], n ≤ 4)`, `pa_status(str)`, `pa_menu(labels[], n ≤ 6)`, `pa_alert(str)`, `pa_confirm(str)` → answer arrives as an event | — |
| Input | `pa_graffiti(mode)`: off / digits / letters | — |
| Time | `pa_now()` (epoch, 64-bit), `pa_localtime(epoch, pa_tm *)`, `pa_ticks_ms()`, `pa_timer(ms)` (one repeating `TICK`, ≥ 100 ms) | `PA_CAP_TIMER` |
| Random | `pa_random()` (hardware RNG) | — |
| State | `pa_state_save(ptr, len, ver)`, `pa_state_load(ptr, len, ver)` → size, version and CRC are checked by the firmware and written with `safefile` | — |
| Files (own folder only) | `pa_file_open(name, mode)` → handle (max 2 open), `pa_file_read/write/seek/size/close`. Names are `[a-z0-9._-]{1,31}`, with no `/`, no `..` and no absolute paths. Writes are limited to **256 KB per app** | `PA_CAP_FILES` |
| Memory | `pa_arena(bytes)`: **one** bump allocation per session, freed automatically on close. There is no `malloc` or `free`. | — |
| Helpers (header-only) | `playclock.h` (already pure), `pa_fmt_int`, `pa_fmt_time`, a bounded `pa_strlcpy`/`pa_strlcat`, fixed-point helpers | — |
| Logging | `pa_log(str)` → serial console, and the simulator's stdout | — |

Deliberately left out of v1: raw LVGL, PIM data access (a later
`PA_CAP_PIM_READ` could be granted with an on-screen prompt at first launch),
networking, NVS, sound (no hardware for it yet), and threads.

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
    pa_clear();
    for(int i = 0; i < S.n; i++){
        int x = 20 + i * 72;
        pa_rect(x, 50, 56, 56);
        char buf[4];
        pa_fmt_int(buf, sizeof buf, S.face[i]);
        pa_text(x + 22, 68, PA_FONT_BOLD, buf, sizeof buf);
    }
    pa_invalidate();
}

static void roll(void){
    for(int i = 0; i < S.n; i++) S.face[i] = (uint8_t)(1 + pa_random() % 6);
    S.rolls++;
}

static void on_event(const pa_event *e){
    static const char *const BTN[] = { "1", "2", "3" };
    switch(e->type){
    case PA_EV_OPEN:
        if(pa_state_load(&S, sizeof S, STATE_VER) != sizeof S){ S.n = 2; roll(); }
        pa_title("Dice");
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
wrappers, and returns the descriptor.

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
| No `volatile`, inline asm, or integer-to-pointer casts | No hardware access or firmware poking outside the API | Lint (G0) plus the instruction scan (G6) |
| Event handlers return in **≤ 50 ms** (the soft budget); long work is split across `TICK`s | Keeps the UI and the task watchdog alive | Instruction count per event in QEMU (G7); a device-side watermark (§8) |
| Pure logic lives in `logic.c`/`logic.h` with **no `pa_*` calls**, plus a `test_logic.c` | The same pattern as `sudoku.c`/`sim/tests/sudoku_test.c`: the hard part is unit-testable on a desktop | Required files, checked by `packlint`; the test must pass (G3) |

### 6.2 What an AI author gets

In the `cyd-palm-apps` template:

- **`AGENTS.md`** (with `CLAUDE.md` pointing to it): the rules above as a
  checklist, the one command to run, how to read its output, the memory and
  time budgets in plain numbers, and "do not" items (no `malloc`, no LVGL, no
  recursion, no string building without sizes).
- **`sdk/docs/API.md`, generated from `palm_app.h`**, where every function
  has a one-line contract saying what it clips, what it truncates, and what it
  returns on error. The header is the authority, so the docs can't drift.
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
  branch. CI runs every gate, and on success attaches an unsigned pack and
  screenshots to the run, so they can be tried in Developer Mode.

---

## 7) The validation pipeline

Every gate runs in CI through **one reusable workflow**,
`cyd-palm-bridge/.github/workflows/validate-app.yml` (`workflow_call`),
pinned to the app's `sdk-vA.B` tag, so all apps get the same checks. G0–G5
also run locally with `make check`.

| Gate | What it runs | What it catches | Fails when |
|---|---|---|---|
| **G0 Policy lint** | `packlint --source`: includes, banned identifiers, globals, required files, manifest sanity | Out-of-profile code (§6.1) | Any violation |
| **G1 Strict compile, twice** | Host `clang` **and** Xtensa `gcc` (the IDF 5.5 toolchain) with `-std=c11 -Wall -Wextra -Wconversion -Wshadow -Wvla -Wformat=2 -Wcast-align -Wstrict-prototypes -Werror` | Truncation, sign mixups, shadowed state, misalignment | Any warning on either compiler |
| **G2 Static analysis + stack bound** | `gcc -fanalyzer`, `cppcheck --enable=warning,portability`, `clang-tidy` (`bugprone-*`, `cert-*`, `clang-analyzer-*`); `stackcheck.py` over `.su` and `.ci` files | Null dereferences, out-of-bounds indexes, uninitialised reads; recursion; stack over 2 KB | Any finding not waived in `app.toml` with a reason; any call cycle; stack > budget |
| **G3 Logic tests** | `test_logic.c` built with ASan + UBSan, **`-m32`** so type sizes and struct layout match the device | Wrong game rules and edge cases, before the UI is involved | Any failed check or sanitizer report |
| **G4 Simulator under sanitizers** | The real firmware UI (`sim/`, 32-bit, LVGL pool and heap cap at device values) with the app compiled in through the same `palm_api.c`; **ASan + UBSan** (including `-fsanitize=alignment`). Runs (a) the app's scripted `tour.txt`, (b) **a monkey run**: 20,000 random pen, button, menu and char events across 8 fixed seeds, with open/close cycles, (c) **a state-file fuzz**: truncated, oversized and bit-flipped `state.bin` and data files fed to `OPEN` | Out-of-bounds reads and writes on app state, overflow UB, unaligned access (a hard fault on LX6), crashes on unexpected input order, trust in on-disk bytes | Any sanitizer report, crash or hang (10 s per event) |
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
- **The API checks its own arguments.** Coordinates are clipped, lengths are
  capped, handles are range- and generation-checked, and file names are
  checked against the allowed pattern. A misbehaving call returns an error
  code; it never faults.
- **No apps during HotSync or a Coach seal.** `apphost` refuses to open an
  app while a sync is running, and closes the current app (saving it) before
  a sync starts or a Coach seal begins. (A seal already makes games
  unreachable; check in Phase 3 whether HotSync does the same, or add it.)

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
| **Malicious pack reading the NVS passwords** | **No: mitigated, not prevented** | Signing (only CI-validated packs load by default) and the G6 instruction/address scan make it hard. A determined author can still compute addresses at run time. **The plain ESP32 can't isolate an app from the firmware.** The real fix is flash encryption plus NVS encryption, which is one-way on this chip and a separate decision. |

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

### Phase 1 — Versioning groundwork (can run alongside Phase 0)

- Take the firmware version from `git describe` (`PROJECT_VER`) and show it,
  with the API range, in Settings ▸ About. Make CI's `package_firmware.py`
  use the same string.
- Add `sdk/` with a first `palm_app.h` (marked unstable, API `0.x`), the ABI
  freeze script, and `abicheck` in CI.
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
- Examples: `hello`, `dice`, `counter`. `make check` passes on all three.
- **Exit:** all three examples pass G0–G5 locally and in CI, and the Sudoku
  port compiles against the API (it doesn't have to pass yet).

### Phase 3 — Device app host

- `apphost.c` and `packfmt.c`: verify, load, run, unload, the guards from §8,
  quarantine, crash log, Developer Mode, and the release public key.
- The More folder lists `/sdcard/apps/*/app.pack` tiles from their headers.
- G7 (QEMU on the real loader) goes into CI.
- **Exit:** the three examples pass G7. A deliberately broken test pack (an
  overrun, an infinite loop, a null dereference) is **quarantined, and the
  device boots to the launcher** in QEMU. A pack whose signature is wrong is
  refused. Heap after 50 open/close cycles is equal to heap at boot.

### Phase 4 — Split out the apps repo, and freeze the API

- Create the `cyd-palm-apps` template: `AGENTS.md`, `CLAUDE.md`, `make new`,
  `make check`, `make shots`, the lock file, and a CI workflow that calls
  `validate-app.yml`.
- Move the examples there, and add `reader` and the `sudoku` port.
- Set up the release key: the public half goes in the firmware, the private
  half becomes a secret in the apps repo. Make G8 the release job.
- Freeze **`sdk-v1.0`**: API `1.0` and `abi/pa_api_v1.txt`. Add the app
  compatibility gate to this repo's CI.
- **Exit:** a fresh Claude Code session given only the `cyd-palm-apps` repo
  and a one-paragraph app idea produces an app that passes every gate
  **without human edits**. Try three different ideas. Any failure is a gap
  in the docs or SDK, and gets fixed there, not in the app.

### Phase 5 — On glass

- Bench checks on real hardware: install from the card, the SD-mounted heap
  figures (replacing the estimate), tile draw time with 10 packs, the
  quarantine path after a real panic, and the Sudoku port's speed next to
  the built-in game.
- **Exit:** these are recorded in `BUILD_PROGRESS.md` and the `BACKLOG.md`
  bench list is cleared.

### Later (not planned yet)

- `PA_CAP_PIM_READ` (read-only Date Book, To Do and Memo access, with a
  first-launch prompt).
- Downloading packs during HotSync from the apps repo's releases.
- Moving a built-in game out to a pack to shrink the image.
- Flash and NVS encryption (§9).

---

## 11) Decisions we need to make

1. **Separate `cyd-palm-apps` repo from Phase 4 (recommended), or `apps/`
   inside this repo** until the API freezes? A separate repo is what makes
   the version split real. Keeping it inside is less setup for the first few
   weeks.
2. **Licence for the SDK and apps.** The firmware is GPLv3 because of the
   PumpkinOS assets. Packs that load into it are probably GPLv3 derivatives
   unless `palm_app.h` carries an explicit exception (like the Linux syscall
   note). Recommendation: license `sdk/` under MIT, add a "linking via
   `palm_app.h` does not make an app a derivative work" exception, and choose
   the apps repo's licence freely. This needs a decision, not just code.
3. **Signing from day one (recommended), or Developer Mode only** until
   Phase 4? Signing costs little and makes "only validated packs load" the
   default from the first pack.
4. **API surface for v1.** The table in §5 is the proposal. Anything added
   after `sdk-v1.0` has to stay forever (append-only), so it's better to
   leave out something doubtful now and add it in `1.1`.
5. **Whether to encrypt NVS** (flash encryption) before encouraging anyone
   else to write apps. It's a one-way change on this chip. It isn't required
   for the "only my own apps" use.
