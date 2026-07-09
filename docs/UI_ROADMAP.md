# Making it a real PDA — UI + Graffiti on the no-PSRAM CYD

Goal: a cohesive, usable handheld — browse/edit DateBook, Address, ToDo, Memo on
a touchscreen with Graffiti text entry, sync to iCloud, run on a battery, live in
a printed case. This doc answers the up-front question — **does 0 PSRAM block
this?** — then lays out the build.

> **STATUS (2026-07-09) — most of this is built and on-device; sync is now
> two-way.** Done + on hardware: U0 (static→heap RAM unlock), U1 display, U2 touch
> (calibrated, NVS), U3 app shell, U3a Palm fonts/icons/theme, U4 data views, U5
> detail+edit, **F1 menus, F2 categories, F3 Memo/apps, F4 Details, U6 Graffiti,
> U7 HotSync (BIDIRECTIONAL iCloud sync working for Date Book + To Do + Address —
> push & pull, edits survive, deletes propagate), per-record Delete + confirm,
> Calculator app**. TLS fits at `MAXR=24` (an earlier 24→96 bump broke pulls). See
> `docs/NEXT_STEPS.md` for what's next (UID-based matching, >24-record streaming,
> on-device config.ini, Graffiti punctuation, Find UI). **Remaining:** U8 power,
> U9 case (hardware); Graffiti training-game app + X 2-stroke (backlog below).
> Blow-by-blow + commit list in **docs/BUILD_PROGRESS.md** (the cold-resume record).

> **BACKLOG — Graffiti training "game" app.** Real Palm Pilots have a Graffiti
> icon in the launcher; we should too. Make it a light, kanji/Chinese-writing-app
> style trainer: show the target stroke, user traces it, score against the
> template, advance. Doubles as a per-user *training* mode — capture the user's
> own stroke as the template so recognition calibrates to this exact resistive
> panel. Case model is settled: **one stroke set (26 letters, drawn like
> capitals), lowercase output, an upstroke = shift-next-char to uppercase** (two =
> caps lock) — authentic Palm, no separate uppercase templates.

## TL;DR verdict

**Not blocked.** The base CYD (ESP32-D0WD, 520 KB SRAM, ~320 KB usable DRAM, no
PSRAM) can run this, because a Palm-style PDA avoids the only two things that
actually overflow the budget:

1. **No full framebuffer.** A 320×240×16bpp frame is 150 KB — that alone won't
   coexist with the Wi-Fi/TLS stack. We never allocate one. We draw in **partial
   strips** (~20–40 KB of draw buffer), which is how every MCU GUI without PSRAM
   works. The screen is small and mostly static (lists, forms) — partial
   rendering is a perfect fit and doesn't hurt the feel.
2. **Rich UI and TLS never run at the same instant.** The Palm model *is* our
   model: use it offline, HotSync periodically. During interactive use Wi-Fi is
   **down** and the full heap is for the UI; during a sync the screen shows only
   a progress line. We **time-multiplex** the two heavy consumers instead of
   summing them.

The device already boots, syncs to live iCloud, and holds ~78 KB free heap
*during* a TLS sync **with no display driver loaded**. The UI work fits in the
headroom that opens up once Wi-Fi is off. Details below.

---

## The hardware (facts)

- **ESP32-D0WD-V3**, 520 KB SRAM: ~320 KB DRAM for data (rest is IRAM for code),
  4 MB flash, **no PSRAM**. Measured free heap: **129 KB at boot**, **78 KB
  during a TLS sync** (Wi-Fi + mbedTLS up), headless.
- **Display**: 2.8" 320×240 ILI9341 (some CYD units ST7789), SPI. Pins (typical
  ESP32-2432S028R): SCLK 14, MOSI 13, MISO 12, DC 2, CS 15, BL 21 — on one SPI
  host (HSPI).
- **Touch**: XPT2046 resistive, its **own** pins (CLK 25, MOSI 32, MISO 39, CS
  33, IRQ 36) — independent of the display bus.
- **SD**: SCLK 18, MOSI 23, MISO 19, CS 5 — a **third** set of pins. (We put SD
  on SPI2 for the sync firmware; the display then takes SPI3, or we reassign.
  The point: **TFT, touch, and SD are three separate SPI arrangements** and don't
  contend — good.)
- **Battery**: JST header + a divider on GPIO34 for voltage sense.

---

## RAM budget — the actual analysis

Two mutually-exclusive modes. The rule that makes the whole thing fit:
**never hold LVGL's big draw buffers and the TLS handshake at the same time.**

### Mode A — Interactive (Wi-Fi OFF)
| Consumer | KB |
|---|---|
| FreeRTOS + IDF baseline + stacks | ~55 |
| Graphics draw buffers (2 × 320×32×2, partial) | ~40 |
| Graphics core + view object tree (lists/forms) | ~30 |
| One PDB record streamed for display | ~5 |
| Graffiti recognizer + stroke point buffer | ~3 |
| **Used** | **~135** |
| **Free (of ~275 usable)** | **~140** |

Comfortable. Wi-Fi is de-initialized here, so its ~50 KB is reclaimed.

### Mode B — Sync (Wi-Fi ON + TLS)
| Consumer | KB |
|---|---|
| FreeRTOS + IDF baseline + stacks | ~55 |
| Wi-Fi driver + lwIP | ~50 |
| mbedTLS handshake peak (IN_CONTENT 16 KB) | ~40 |
| Sync engine working set (heap; see note) | ~55 |
| **Progress screen** (small line buffer, direct SPI — no LVGL) | ~4 |
| **Used** | **~204** |
| **Free (of ~275 usable)** | **~70** |

Matches what we already measured headless (78 KB free); the ~4 KB status buffer
barely moves it. Works.

### The transition (the one discipline that matters)
`UI mode → tear down LVGL big buffers → esp_wifi_init → sync (progress screen via
direct SPI) → esp_wifi_deinit → rebuild UI`. Never overlap the two peaks.

### Note: make the sync working set heap, not static
Today the sync arenas are `static` (BSS), so they're resident **even in UI mode**,
taxing the interactive budget by ~55 KB for no reason. Converting them to
`malloc`-on-sync / `free`-after (the "proper" B3 fix we deferred) hands that
55 KB back to the UI. **This is a prerequisite for the UI, not optional.** It's a
mechanical change (the buffers already have a clear lifetime).

### What WOULD be blocked (and why we don't need it)
- A **full-framebuffer, double-buffered** GUI (300 KB of buffers) — no. We use
  partial buffers.
- **Rich animated UI while streaming over TLS** — no. Sync shows a progress line,
  not the app.
- **Fonts/assets held in RAM** — no. Fonts and Graffiti templates live in **flash**
  (const), glyph cache is a few KB.

Neither of the first two is part of a Palm-style PDA. The constraint shapes the
design; it doesn't break it.

---

## Graphics stack — DECIDED: LVGL, skinned with real Palm assets + layouts

**Decision:** render with **LVGL** (partial buffers, ~40 KB), but make it *look*
authentically Palm by **reusing as many Palm assets as practical** and
**reproducing PumpkinOS's actual screen layouts**. Fast/low-risk engine,
authentic skin. Reusing GPLv3 Palm assets makes the **firmware GPLv3** (accepted).

- **Fonts** — convert the Palm system bitmap fonts (from PumpkinOS resources) to
  LVGL fonts (`lv_font_conv`). Authentic Palm glyphs are the single biggest
  "feels like a Palm" win and live in flash (glyph cache is a few KB).
- **Icons / chrome** — extract Palm app icons, checkbox/scrollbar/button bitmaps,
  the silkscreen/title-bar look from PumpkinOS resources → LVGL image assets.
- **Layouts** — use PumpkinOS's Form definitions (its `.rcp`/`FrmXXX` layout code)
  as the **spec** for each screen: DateBook agenda/day, Address list/edit, ToDo,
  Memo, dialogs, menus — place LVGL widgets at the authentic Palm coordinates and
  proportions so structure matches, even though LVGL draws it.
- **LovyanGFX / direct drawing** — kept as a fallback only if LVGL proves heavy.
- **Sync-mode progress screen**: direct SPI writes, no big buffer while TLS is up.

Asset pipeline is its own step (U3a below); layouts are referenced throughout
U4–U5. Note: the earlier 160×160-framebuffer analysis still applies if we ever
want a Palm-native logical surface, but LVGL partial rendering is the chosen path.

---

## PumpkinOS: what we donate, and what we don't

PumpkinOS is a donor codebase, not a runtime. With **GPLv3 accepted**, we now use
it three ways (the firmware is therefore GPLv3):

- **Data formats — already banked.** `pdb/datebook/address/todo.c` are clean-room
  from the documented Palm layouts, with PumpkinOS's `DateDB.c`/`AddressDB.c` as
  the format oracle (we even fixed a bug its `ApptUnpack` has).
- **Assets — reuse (new).** Palm **system fonts** → LVGL fonts; **icons/chrome**
  bitmaps → LVGL images (U3a). These are the authentic-look payload.
- **Layouts — reference (new).** PumpkinOS's Form definitions are the **spec** for
  our LVGL screens (authentic coordinates/structure), U4–U5.
- **Algorithms — reference.** `libpumpkin/Find.c` for the streaming Find (U4);
  date math.
- **Not ported:** PumpkinOS's *rendering runtime* (SDL2 blit, PACE, module loader,
  the `WinXXX` window system) — we render with LVGL instead, so this desktop
  baggage never comes along. **Graffiti** is stubbed in PumpkinOS; we bring $Q
  (U6).

So we lift the *portable value* (formats, fonts, icons, layout specs, algorithms)
without porting the framebuffer-coupled runtime. The look is authentic; the engine
is LVGL.

## Palm FUNCTIONAL design — the priority (leverage, don't reinvent)

Direction (2026-07-04): pixel/font fidelity is secondary; the goal is to leverage
PalmOS's **functional, user-centric design** — its menus, feature set, and
interaction patterns — rather than reinventing usability by trial and error.
Ground truth: the **PalmOS Programmer's Companion UI reference**
(https://palm.wiki/development/docs/601/PalmOSCompanion/UserInterface.html) for
conventions, and the **PumpkinOS app sources** (scratchpad/PumpkinOS/src/{DateBook,
AddressBook,ToDoList,MemoPad}) for each app's actual menus/forms/features. Replicate
these; don't invent.

**F1 — Menu system (central Palm pattern; currently missing).** Tapping the title
bar (Palm OS 3.5+) opens the menu bar with pull-downs. Per app: **Record** (New,
Delete, Duplicate, Beam), **Options** (Font, Preferences, Categories, About),
**Edit** (Cut/Copy/Paste in text fields). Implement as an LVGL overlay driven off
a per-screen menu descriptor.

**F2 — Categories (we already have the codec).** Category picker = a **pop-up
trigger top-right of the title bar** showing the current category; list = All +
the AppInfo categories + Edit Categories. Filters the list view. Wire to the
existing `appinfo.c` AppInfo table (max 16) + record attr nibble — the data half
is done; this is UI over it.

**F3 — Per-app views, per PalmOS + PumpkinOS:**
- **To Do**: multi-column rows = checkbox · priority · text · due; Options: sort
  (priority/due/category combos), Show Completed, Show Only Due, show/hide
  due/priority/category columns; Details dialog (priority, category, due, private).
- **Date Book**: Day view (time grid), Agenda, Week; Go to Date; the New-at-time
  and untimed-event patterns; alarms/repeat in Details.
- **Address**: Name+phone list filtered by category; record view → Edit; Details
  (category, private); the labeled multi-phone/show-in-list convention.
- **Memo Pad**: list of memo first-lines by category; sort; the plain-text editor.

**F4 — Details dialog + record form conventions.** Record edit form bottom row =
**Done / Details / New** (Palm layout, not our current Done/Edit). Details =
modal dialog (category trigger, private, per-app fields) with **Delete/Note/Beam**
in its menu; **OK right, Cancel left**. Dialogs follow Done/OK-right, Cancel-left.

**F5 — Standard Palm controls.** Pop-up triggers (label+▾ opening a list),
selector triggers (gray frame opening a picker, e.g. dates), checkboxes, push-
button groups (view switching), Palm button behavior (highlight while pressed).

This reframes U4/U5 (which built basic list/detail/edit scaffolding) toward Palm's
real information architecture. Suggested order: **F1 menus + F2 categories** first
(they touch every app and unlock the command/filter model), then **F3** app by app,
with **F4/F5** controls filled in as needed. U3a (skin) continues in parallel/light.

## Phased plan

**U0 — Sync working set: static → heap.** Prerequisite (frees ~55 KB for UI).
Guarded so host build is unchanged. Re-run all 5 gates.

**U1 — Display bring-up.** ILI9341/ST7789 driver, SPI host assignment that
coexists with SD, backlight on GPIO21. Draw a test pattern + text. Confirm which
panel/controller this unit has.

**U2 — Touch.** XPT2046 read + a 5-point calibration stored in NVS. Crosshair
calibration screen. Map raw → screen coords.

**U3 — Graphics stack + app shell.** Bring up **LVGL** (partial buffers). A
launcher (DateBook / Address / ToDo / Memo / Sync) and a nav model (app → list →
detail → edit), themed for a crisp Palm feel.

**U3a — Convert to authentic PalmOS look (Palm assets from PumpkinOS).**  ← the
skinning pass; NOT optional. U3–U5 built the UI functionally with LVGL's *default*
Montserrat font and default theme — placeholder styling. U3a replaces that skin
with the real Palm look, per the locked decision (commit 357f044): LVGL engine,
authentic Palm assets + PumpkinOS layouts, firmware GPLv3. The functional UI
already in `ui.c` is the base to re-skin (title bar, launcher list, list/detail/
edit views, Graffiti strip). Concrete steps:
- **U3a.1 Fonts.** Lift the PalmOS system bitmap fonts from PumpkinOS
  (`libpumpkin` font resources) and convert to `lv_font` with `lv_font_conv`
  (bitmap mode, subset to Latin-1). Wire as the default LVGL theme font +
  a bold variant for the title bar. This is the single biggest "feels like a
  Palm" change (the crisp Palm glyphs vs Montserrat).
- **U3a.2 Chrome + icons.** Extract Palm UI bitmaps (app-launcher icons for
  DateBook/Address/ToDo/Memo/HotSync, checkbox, scrollbar, button, the
  title-bar/silkscreen look) from PumpkinOS resources → LVGL image assets;
  swap the placeholder `LV_SYMBOL_*` and list bullets for them.
- **U3a.3 Theme.** An LVGL theme matching Palm: the Palm font, black 1-px
  borders (no rounded corners — already radius 0), the inverted-bar title style,
  flat gray/mono palette, Palm-proportioned list rows and buttons.
- **U3a.4 Apply across screens.** Re-skin launcher, list views, detail, edit
  forms, and the Graffiti strip with the theme + assets; match spacing to the
  PumpkinOS Form layouts. Add a `LICENSE` (GPLv3) + asset-provenance note.
Keep the option to render a Palm-native 160×160 low-color logical surface if we
want pixel-exact Palm rendering later (the framebuffer fits — see RAM analysis).

**U4 — Read-only views (data plane done), laid out per PumpkinOS.** Stream
records from the PDB on SD into DateBook agenda/day, Address list + detail, ToDo
list, Memo list + viewer — each screen's **layout referenced from PumpkinOS's
Form definitions** (authentic positions/structure). Global Find (streaming
search), Find.c as the reference. Codec + PDB reader are ready; this is UI +
skinning over existing data.

**U5 — Editing (authentic forms).** Record new/edit/delete forms matching the
Palm Form layouts, setting the Palm dirty bit so edits flow back through the
**already-proven** sync engine. On-screen keyboard first (works before Graffiti).

**U6 — Graffiti.** Ink capture surface + the **$Q recognizer** (built for MCUs,
~1–2 KB working set; templates in flash). PumpkinOS stubs Graffiti, so we bring
our own. Punctuation/shift strokes, a small preview. This is the signature
feature and its own mini-project.

**U7 — Sync integration + mode switch.** The A↔B transition (U0 makes it safe):
a Sync screen with progress, trigger by button/menu or a periodic timer, and a
conflict-resolution UI on top of the existing server/local/keep-both engine.
On-device discovery + category routing (port `resolveColls`/`synccat` logic from
the host CLI) so the user picks calendars on the device.

**U8 — Power (for the battery goal).** Light-sleep between touches, dim/off
backlight on idle, Wi-Fi only during sync, battery gauge from GPIO34, low-battery
UI. Target: days of standby, hours of active use on a ~1100 mAh LiPo.

**U9 — Case/hardware integration.** Confirm final pin map, expose USB + SD +
power switch + reset, mount points — folds into the case work you already have
staged for the Cardputer-style process.

---

## Flash budget
4 MB flash, 3 MB app partition today. Current headless bin ~1 MB; LVGL + fonts +
views add ~0.4–0.8 MB → ~1.8 MB, fits the 3 MB partition. If assets grow, carve a
small LittleFS partition for fonts/Graffiti templates. 4 MB is adequate, not
roomy — keep fonts subset-ed.

## Risks / mitigations
- **Panel variance** (ILI9341 vs ST7789 on different CYD batches) → detect/verify
  in U1; keep the init sequence swappable.
- **SPI bus contention** if a future feature shares a bus → keep TFT/touch/SD on
  their three separate pin sets; guard the SD host id.
- **LVGL heavier than budgeted** → drop to LovyanGFX/direct (U3 keeps this open).
- **Sync-mode heap** with a live screen → progress via direct SPI, not LVGL, and
  the U0 heap conversion keeps ~70 KB free during TLS.
- **Graffiti accuracy** on resistive touch → $Q is forgiving; add a keyboard
  fallback (U5) so text entry never depends solely on it.

## Licensing
Reusing PumpkinOS's GPLv3 Palm fonts/icons (and possibly `Find.c`) makes the
**firmware GPLv3** — accepted. The host-side codecs remain clean-room and can
stay separately license-free; the device binary that links Palm assets is GPLv3.
Add a `LICENSE` (GPLv3) and asset-provenance note when U3a lands.

## Open decisions (settle as we go)
- ~~LVGL vs LovyanGFX~~ — **decided: LVGL, Palm-skinned** (assets + layouts reused).
- Sync trigger: physical button vs on-screen vs periodic timer (U7).
- Two-way on-device editing v1, or view + keyboard-edit first, Graffiti after.
- Battery gauge calibration + charge circuit (confirm this unit's charging).

---

## Bottom line for the "battery + case" plan
Build the device. The no-PSRAM limit forces **partial rendering** and a
**sync-vs-UI mode switch** — both authentic to how a Palm actually worked, neither
a compromise on the experience. The data/sync half is done and proven on
hardware; the UI half is a well-scoped build with a RAM budget that closes with
~70 KB to spare in the tight mode and ~140 KB in the roomy one.




## More to do / backlog - check if not yet completed
a) Build out Memo Pad, Date Book, Address, and To Do List apps UI's referencing the PumpkinOS versions.
1) Preferences app with icon from PumpkinOS converted similarly to other icons.  Configure wifi and caldav/carddav settings here.  Configure RSS data sources  by selecting from a preloaded list of 2026-most-popular.  Configure timezone (from list) and clock.  Review PumpkinOS preferences app for Palm UI choices other contents needed here.  Configure screen max brightness and power-saving feature timers.
2) RSS reader app on homescreen with icon, with data loaded via HotSync that formats each article with swipe-up tiktok-style scrolling/flicking. 
3) Change behavior of hardware reset switch to instead control on/off toggle.
4) Clock on top of screen per Palm OS design.
5) Graffiti-area icons: move search to bottom right, calculator above search - build out calculator and search apps leveraging PumpkinOS and palm original design concepts.  Add the context-sensitive menu to all screens per original Palm functionality.
6) Battery % graphic in top menu bar.
7) Stretch goal: dark mode and auto switch at sunset matching timezone/calendar
