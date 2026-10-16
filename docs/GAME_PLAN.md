# Game plan — a monster-catching RPG built from Tuxemon's open content

> **Status: G0 done (2026-10-16), awaiting the owner's review.** The owner
> asked for G0, the file format and the converter, on 2026-10-16:
> `docs/GAME_FORMAT.md` is the format, `tools/mkgame.py` the converter, and
> §12 says what G0 found. **Don't start G1 without the owner's yes**, and
> see §13 Q6 first: Tuxemon's town buildings are under a licence this plan
> leaves out. The sync engine work (`BACKLOG.md` §Engine) is unaffected.

A small Game Boy-style monster-catching RPG in the Games menu: walk a tile map,
meet creatures in the grass, battle, catch and train them, beat trainers. An
**Auto-play** checkbox hands the controls to a simple bot so the owner can
watch it play, as casual entertainment.

The engine is ours, written in C for this board. The **content** (creatures,
moves, types, maps, tiles, characters) comes from
[Tuxemon](https://github.com/Tuxemon/Tuxemon), a free GPLv3 monster-catching
game whose art is mostly CC-BY-SA, converted on a computer into one data file
on the SD card.

## Contents

1. [Why this shape](#1-why-this-shape)
2. [What it does](#2-what-it-does)
3. [Where things live](#3-where-things-live)
4. [Content: what Tuxemon gives us](#4-content-what-tuxemon-gives-us)
5. [The game file on the card](#5-the-game-file-on-the-card)
6. [Rules of play](#6-rules-of-play)
7. [The screen, drawing and controls](#7-the-screen-drawing-and-controls)
8. [Auto-play](#8-auto-play)
9. [Memory, flash and time budgets](#9-memory-flash-and-time-budgets)
10. [Licences and credits](#10-licences-and-credits)
11. [Risks to the firmware](#11-risks-to-the-firmware)
12. [Phases](#12-phases)
13. [Decisions and open questions](#13-decisions-and-open-questions)

---

## 1) Why this shape

What was ruled out, and why (measured 2026-10-11/12; the measuring code was in
the session scratchpad and is gone):

- **A PICO-8 runtime that runs carts unchanged.** Picomon Red (a PICO-8 port of
  Pokémon Red) was measured in Lua 5.4 configured 32-bit like the ESP32:
  - its compiled code alone is 51–70 KB;
  - PICO-8's address space is 64 KB;
  - its start-up decoder builds a Lua table of 32,000 entries, about 262 KB.

  The board has about 105 KB of free heap (largest block about 90 KB). A
  general loader could only run small carts (roughly 2,000–3,000 tokens).
- **Porting Picomon Red's code.** It has no licence (all rights reserved), and
  its data (stats, moves, maps, art) is pulled from the Pokémon Red ROM at
  build time. **Neither its code nor anything from the ROM may enter this
  repo.** This engine is written fresh, from the genre's general ideas and
  Tuxemon's published rules.
- **Inventing all the content ourselves.** Possible, but it's the bulk of the
  work. Tuxemon already publishes creatures, moves, types, encounters and maps
  under licences that fit a GPLv3 repo (§10).

What was chosen:

- **Built in, like Study:** C engine in the firmware, content as data on the
  card. This follows `SRS_PLAN.md` §11 decision 2: one file, built on a
  computer, read a piece at a time, never loaded whole.
- **Streamed by area:** only the current area, the tiles on screen and the two
  creatures in a battle are in RAM. A transition screen covers each load.

## 2) What it does

- **Overworld:** a 16×16-tile map seen from above.
  - The player steps one tile at a time; walls and water block, doors and
    stairs lead to other areas.
  - People can be talked to (a text box) and some battle you.
- **Encounters:** walking on grass, in caves or on water can start a battle
  with a wild creature, chosen by the area's encounter table.
- **Battles:** turn-based, one creature each side.
  - Fight: one of up to four moves. Swap: another creature from the party.
  - Item: heal, or a capture device to catch the wild creature. Run: from wild
    battles only.
- **Growth:** creatures gain experience, level up, learn moves at set levels
  and evolve at set levels.
- **Party and storage:** up to six in the party; the rest are stored. A centre
  in each town heals the party.
- **Saving:** one save slot on the card, written on request and when leaving
  the game.
- **Auto-play:** a checkbox in the game's menu (§8).
- **No sound** in the first version. The board has a small speaker output;
  sound is a later, separate decision.

## 3) Where things live

| What | Where |
|---|---|
| Rules, battle maths, save and load, the area reader | `firmware/main/monster.c` (no LVGL; host-testable, like `zip.c` and `srs.c`) |
| The bot | `firmware/main/monster_bot.c` (no LVGL) |
| Screens, drawing and controls | **a new file**, `firmware/main/ui_monster.c`, not another section of `ui.c` (already 12,340 lines) |
| Entry | `GAMES[]` in `ui.c` (one entry and one dispatch line) |
| The converter | `tools/mkgame.py`: Tuxemon's YAML, PNG and Tiled files to `game.bin` (Pillow and PyYAML pinned in `tools/requirements-game.txt`) |
| Pinned source | Tuxemon at a fixed commit (the full SHA in `mkgame.py`), fetched by `mkgame.py --fetch`; the build refuses any other commit or a changed checkout. **Not vendored** into the repo. |
| The licence allow-list | `tools/game_allow.tsv`: each creature, tileset and character sheet we use, with its licence and credit line, checked by hand (§10) |
| The region | `tools/game_region.json`: the maps, the start, the player's sheet, the encounter tables |
| Stable ids | `tools/game_ids.tsv`: creature, move, status and area numbers a save refers to |
| The data file | `/sdcard/games/monster/game.bin` (built from Tuxemon; see §13 Q1 on how it reaches the card) |
| The save | `/sdcard/games/monster/save.dat` |
| Host gates | `make -C sim monster` (rules, save round trip, area reader, bot finishes) and `monster_fuzz` in `make ftest` |
| Docs | `docs/GAME_FORMAT.md` (the file layout, written in G0) |

## 4) Content: what Tuxemon gives us

Measured on Tuxemon's `development` branch at commit `9e6258f`
(2026-09-28):

| What | Count | Contents |
|---|---|---|
| Creatures (`db/monster/*.yaml`) | 411 | <ul><li>types</li><li>moves learned by level</li><li>evolutions (level and target)</li><li>catch rate</li><li>terrains it lives in</li><li>body shape</li></ul> |
| Body shapes (`db/shape/shapes.yaml`) | 14 | HP, armour, dodge, melee, ranged and speed per shape. **Stats come from the shape and the level**, not from a per-creature table. |
| Moves (`db/technique`) | 274 | power, accuracy, recharge, type, range (melee or ranged), effects such as burn |
| Types (`db/element`) | 13 | a full multiplier table (0.5, 1 or 2) |
| Encounter tables (`db/encounter`) | 36 | creature, level range and rate per area |
| Maps (`maps/*.tmx`) | 263 | Tiled maps with 16×16 tiles. The median is 400 cells (20×20); the largest is 10,000 (100×100). Events are in matching `.yaml` files. |
| Battle sprites (`gfx/sprites/battle`) | 412 | one 128×88 sheet each: a 64×64 front, a 64×64 back and two 24×24 menu frames |
| Text | `l18n/en_US/LC_MESSAGES/base.po` | names and descriptions |

**Colours:** 188 of the 412 battle sheets already use 15 colours or fewer; the
median is 17. Reduced to 15 colours plus transparency, a sample of 48 looked
almost the same as the originals. Each sprite keeps its own palette (§7).

**What we don't take:** Tuxemon's event scripts. They're written for its own
engine (`create_npc`, `start_battle`, conditions on variables). We take map
layouts, warps, encounter zones and trainers, and write a small event set of
our own (§6).

**The first region:** a playable slice chosen from Tuxemon's maps: a home
town, two routes, a cave and a town with a gym, with about 20 creatures from
the allow-list. More areas are added later by rebuilding `game.bin`, without
reflashing.

## 5) The game file on the card

One file, `game.bin`, in the same style as `course.srs`
(`COURSE_FORMAT.md`):

- **Header:** magic, version, a section table, and a CRC per section, so a
  damaged file is refused rather than misread.
- **Sections, each read a piece at a time:**
  - creatures: fixed-size records (types, shape, catch rate, evolution, move
    list offset, name);
  - moves: fixed-size records;
  - the type table: 13 × 13 bytes;
  - battle sprites: per creature, front and back at 4 bits per pixel with a
    16-colour palette (about 4 KB per creature);
  - areas: a directory, then per area its size, its palette, its tile list,
    its cells, collisions, warps, encounter zones, people and text;
  - tiles: 16×16 at 4 bits per pixel (128 B each), stored per area;
  - text: names, descriptions and dialogue, Latin-1 so the Palm fonts draw it.
- **Every map in chunks** of 16×16 cells (3 bytes a cell: a tile and a
  movement byte, so 768 B a chunk). A screen spans at most 2×2 chunks, so
  3 KB of cells are in RAM whatever the area's size (§9). G0 changed this
  from "small maps whole, big ones in 32×32 chunks": one rule is simpler, and
  smaller.
- **Reproducible:** `mkgame.py` rebuilds byte for byte from the pinned
  Tuxemon commit. Its `--check` mode and a dump that the C reader's test dump
  must match, as with courses.
- **Ids are stable:** creatures and moves keep Tuxemon's slugs mapped to
  numbers through an `ids.tsv`, as courses do, so a save survives a rebuild
  that adds content.

`GAME_FORMAT.md` has the byte layouts.

## 6) Rules of play

- **Stats** follow Tuxemon: each creature's shape gives six base values, scaled
  by level.
- **Damage** follows Tuxemon's rules (`GAME_FORMAT.md` §2, read from its
  source in G0): the attacker's melee or ranged stat times (7 + level), times
  the move's power and the type multiplier, over the defender's armour or
  dodge. **Accuracy alone decides a miss;** dodge is only a resistance.
  **Integer maths only**, with a seeded generator, so a battle replays exactly
  from its seed in the host test and on the device.
- **Effects:** the first version takes damage, healing, and giving poison,
  burn or recover. Moves with effects we don't support are left out of the
  build, and `mkgame.py` reports how many: **for the spike's 20 creatures, 27
  moves kept and 49 left out.** Each creature still has a move at level 1,
  but the fights will be plainer than Tuxemon's. More effects (enraged,
  splash, multi-attacks) are the first thing to add after G1.
- **Catching:** uses each creature's catch rate and catch resistances from
  Tuxemon, with the target's remaining HP (`GAME_FORMAT.md` §2).
- **Events:** our own small set, enough for the first region:
  - warp (doors, stairs, map edges);
  - talk (show text);
  - trainer (see the player, walk over, battle once, remember the win);
  - heal (centre);
  - give item;
  - set flag and require flag (gates such as "beat the gym before the route
    opens").
- **Story:** a short one written for this game in the first region's text, not
  Tuxemon's script.

## 7) The screen, drawing and controls

The game runs **full screen** (`ui_full_screen()`, like Study's lessons): the
title bar keeps Home and Menu, and the Graffiti strip is hidden. Below the
24 px title bar, 296 px are left:

| Band | Size | Contents |
|---|---|---|
| Game view | 240 × 176 | 15 × 11 tiles on the map; the battle scene in a battle |
| Controls | 240 × 120 | D-pad on the left, A and B on the right, a Menu button |

- **The game view:** with 8-bit tiles, either an 8-bit indexed canvas
  (42 KB) or, without a canvas, tiles drawn straight into LVGL's draw buffer
  from a cache of RGB565 tiles. G2 measures both (§9).
- **Palettes** (Q4, settled in G0 subject to review: `GAME_FORMAT.md` §4):
  - On the map, **8 bits per pixel**, one palette per area of up to 255
    colours, shared with the character sprites. The spike's areas use 65 and
    33 colours, so they're exact. At 4 bits the encounter grass lost its
    colour and looked like the trees, which the game can't afford.
  - In a battle, each creature sprite is its own 4-bit image (64×64, 2 KB a
    frame) with its own 15 colours, drawn over a plain background. Reduced
    sprites looked the same as the originals.
- **Drawing only what changed:** a step scrolls the view by one tile. The
  first version redraws the view per step in four 4-pixel frames; G2 measures
  the frame rate in the simulator and on the bench, and falls back to
  one-frame steps if it's slow.
- **Controls:**
  - The touch panel registers one touch at a time, so there are no
    combinations: one press is one step or one choice.
  - Holding a direction keeps walking.
  - In menus and battles the D-pad moves a cursor and A chooses; B goes back.
- **Text** uses the Palm fonts in a box at the bottom of the game view.

## 8) Auto-play

- **The checkbox** is in the game's menu. While it's on, a small "AUTO" label
  shows in the title bar, and the bot presses the same virtual buttons a
  person would, at a watchable pace (about 4 steps a second).
- **Touching the screen** turns Auto-play off and gives control back.
- **The bot** (`monster_bot.c`, no LVGL):
  - follows a list of goals for the region (reach the route, beat a trainer,
    heal below a threshold, catch a creature it doesn't have, beat the gym);
  - finds its way with a breadth-first search over the area's walkable cells,
    using warps between areas;
  - in a battle, picks the move with the highest expected damage (power ×
    accuracy × type multiplier), swaps out a creature at low HP, and heals or
    runs when the party is weak;
  - grinds on grass when the party's level is below the next goal's.
- **The bot is also a test.** The host gate runs the bot from a fixed seed and
  checks that **it finishes the region** within a step limit. A map change that
  blocks the way, or a fight that can't be won, fails the gate.
- **The screen during Auto-play:** see §13 Q3.

## 9) Memory, flash and time budgets

Heap, all allocated when the game opens and freed when it closes, in as few
blocks as possible (one if it fits):

Revised in G0 for 8-bit map tiles (`GAME_FORMAT.md` §4); the first plan
had a 4-bit canvas and came to about 45 KB.

| What | Without a canvas | With an 8-bit canvas |
|---|---|---|
| Game view | none: tiles drawn into LVGL's draw buffer | 42,240 B (240 × 176) + 1 KB palette |
| Tile cache, 64 tiles | 32,768 B (RGB565, 512 B each) | 16,384 B (8-bit, 256 B each) |
| Cells: four 16 × 16 chunks | 3,072 B | 3,072 B |
| The player's 12 frames | 6,144 B | 6,144 B |
| The area's head (palette, warps, zones, signs, overlays) | up to about 3 KB (2.4 KB for Brideswood) | the same |
| Game state (party, storage, flags, position) | about 2–4 KB | the same |
| Battle: two sprites, 4.1 KB each | in the tile cache's space | the same |
| Bot (BFS over a 64 × 64 area) | about 8.5 KB, only while Auto-play is on | the same |
| **Total** | **about 48 KB playing; about 57 KB with Auto-play** | **about 74 KB; about 83 KB** |

The 64-tile cache is twice what the spike needs: the most different tiles on
one screen, scrolling included, was 34 (Route 1).

- The device has about 106–108 KB free with the launcher showing and the card
  mounted (largest block about 90 KB), so about half of it is left.
- **No new static DRAM.** Static DRAM is about 89 % used; everything above is
  heap.
- **The LVGL pool:** the game uses a handful of objects (the canvas, the
  buttons, two sprite images, a label). G2 measures the pool with
  `make -C sim poolparity` and the smoke tour.
- **Recommendation:** no canvas, if G2 measures it fast enough. 74 KB with
  a canvas is too near the largest free block (about 90 KB).
- **Opening fails safely:** if the block can't be allocated, for example during
  a sync, the game says so and doesn't open, as Study does.
- **Flash:** the engine and screens are estimated at 40–60 KB. The app
  partition is 3 MB and the image is 1.58 MB, so about 1.4 MB is free.
- **The card:**
  - The whole of Tuxemon's creatures at 4,136 B each would be about 1.7 MB.
  - The spike's file (two areas, 20 creatures) is 132,360 B, of which the
    sprites are 82,720 B. An area at 8 bits is 19–26 KB on the card.
  - Entering an area reads its head (under 3 KB) and four chunks (3 KB);
    tiles are read as they're needed.
- **Time:** G2 measures one step's redraw and one area load, in the simulator
  and on the bench.

## 10) Licences and credits

- **Tuxemon's code and data files** (the YAML databases, the maps) are under
  the GPLv3, the same as the firmware.
- **Tuxemon's art** is mixed:
  - the creature sprites are almost all CC-BY-SA 4.0 (a few are CC-BY-SA 3.0,
    CC-BY or public domain);
  - CC-BY-SA 4.0 is one-way compatible with the GPLv3, which allows it in this
    repo's output, with credit.
- **The allow-list** (`tools/game_allow.tsv`) decides what gets built:
  - Tuxemon's `ATTRIBUTIONS.md` names only about 65–77 of the 411 creatures;
    the rest are credited on the Tuxemon wiki.
  - `mkgame.py` includes **only** what's in the list, each line with its
    licence, authors and source link, confirmed by hand.
  - The OpenGameArt release "Tuxemon Set 1" (154 creatures, CC-BY-SA 4.0)
    may confirm more in one place.
- **Measured in G0: what the XYG rule removes.** `ATTRIBUTIONS.md` says
  four of Tuxemon's shared "core" tilesets are built partly from Kelvin
  Shadewing's XYG-licensed sets: `core_buildings`, `core_set pieces`,
  `core_indoor_walls` and `core_indoor_floors`. It doesn't say which tiles,
  so the whole sheets are out. (The licence's own page wasn't reachable to
  read: its certificate is for another host.) Counted over all 263 maps,
  with only the five clean core sheets allowed (`core_outdoor`,
  `core_outdoor_nature`, `core_outdoor_water`, `core_city_and_country`,
  `core_indoor_stairs`):
  - **40 maps lose nothing.** They're routes, forests, caves and tunnels, no
    towns and no interiors: Spyder's Route 4, 6, A, C and D, Brideswood,
    Diamond Hill, Candy Port, the tunnels, Dragon's Cave, Dryad's Grove, and
    Eclipse's routes F, G and H. (The eleven `classic_*` maps are clean but
    empty, plain grass.)
  - **Spyder's Route 1 loses 2 of its 1,553 tiles** (a signpost's).
  - **Every town loses its buildings.** Paper Town loses 141 of 1,511 tiles,
    which are every house, the centre and the shop. The interiors lose
    nearly everything.

  So routes are fine, and **towns are an open question for the owner
  (§13 Q6).**
- **Creatures in G0:** `ATTRIBUTIONS.md` names 76 creatures, most under
  CC-BY-SA 4.0, a few with no licence given.
  Route 1's own Tuxemon encounter table uses mostly creatures it doesn't
  name, so the spike's region has its own table (`game_region.json`) and 20
  creatures with whole evolution lines credited: the Agnite, Cardiling,
  Vamporm and Anoleaf lines, Eyenemy, Noctula and Selket and their
  evolutions, Zunna and Pigabyte.
- **Character sheets:** the credits don't name files, so the spike's player
  (`adventurer`) is marked `unverified`, and G3's build refuses that.
- **Left out entirely:**
  - the tilesets under the "XYG Open Source License", and the four core
    sheets built from them (above);
  - the LimeZu sets, marked "purchased", since sharing them isn't clearly
    allowed;
  - anything with a non-commercial or no-redistribution term.
- **Credits:**
  - `NOTICE` gains a section for the game data and art, generated from the
    allow-list;
  - the game has a Credits screen, read from `game.bin`.
- **Nothing from Pokémon or Picomon Red:** no names, data, code or art.
- **The game's name** isn't "Tuxemon" unless the owner wants that (§13 Q2).

## 11) Risks to the firmware

| Risk | Size | What keeps it small |
|---|---|---|
| Static DRAM | low | no new statics; the existing gates catch a slip |
| Running out of heap mid-game | low | one allocation at open; refuses to open rather than half-open; G2 measures it alongside a sync in QEMU |
| `ui.c` growing | low | the screens go in their own file |
| Battery and screen dimming | medium | the game must not call `lv_display_trigger_activity()` to stay awake (`BUILD_PROGRESS.md`, the flash lesson); §13 Q3 decides the behaviour |
| Frame rate | medium | measured early in G2; one-frame steps as a fallback |
| A damaged or hostile `game.bin` | low | CRCs plus `monster_fuzz` under ASan and UBSan, like `course_fuzz` |
| Licence mistakes | medium | the allow-list, built by hand and checked in G3 |
| Sync running while playing | low | the game uses only the heap above; the draw buffer already shrinks during a sync |

## 12) Phases

Each phase ends with its gates green and, where there's a screen, smoke
screenshots looked at.

- **G0 — format and spikes. Done 2026-10-16, for the owner's review.**
  - [x] `docs/GAME_FORMAT.md`: the byte layouts, and Tuxemon's rules as G1
    will implement them (§2 there).
  - [x] `tools/mkgame.py` on two real areas (Spyder's Route 1 and
    Brideswood, which join) and 20 creatures: reproducible to the byte, a
    reader with `--check` and `--dump`, previews drawn from the built file.
    132,360 B in all.
  - [x] The allow-list's first lines: five tilesets, 20 creatures, the
    player's sheet (unverified).
  - [x] Q4: 8 bits for maps, 4 for creatures (§7, `GAME_FORMAT.md` §4). The
    renders are drawn from the file by `mkgame.py --preview`, the same
    pixels the device would draw; there's no simulator screen until G2.
  - [ ] The owner reviews the format, the renders and §13 Q6.
  - Not done in G0, on purpose: a host gate for `mkgame.py` (it needs the
    390 MB Tuxemon checkout, so it comes with G1's C reader and a small
    fixture of our own), people (NPCs) and trainers (G3).
- **G1 — rules, on the host.** `monster.c`: the file reader, stats, damage,
  catching, experience, learning moves, evolving, events, save and load.
  Gates:
  - a seeded battle matches a recorded result;
  - save round trip, and every truncation refused;
  - `monster_fuzz`.
- **G2 — screens.** `ui_monster.c`: the map view, scrolling, the text box, the
  battle screen, the controls, the transition screen. Gates: smoke
  screenshots, pool parity, heap given back on leaving (the smoke tour's
  `B`/`E` check). Measured: frame rate, area load time, heap during a sync
  (QEMU).
- **G3 — content.**
  - The first region and its creatures through `mkgame.py --check`, reproducible.
  - The allow-list completed.
  - The `NOTICE` section and the Credits screen.
- **G4 — Auto-play.** `monster_bot.c`, the checkbox, the "bot finishes the
  region" gate.
- **G5 — the bench.** The owner's `[d]` items: playability of the controls,
  the frame rate, the battery with Auto-play, a card swap.

Later, separate decisions: more regions, sound, more effects.

## 13) Decisions and open questions

**Decided (2026-10-12):**

1. **Content from Tuxemon,** engine written fresh in C. No Picomon Red or
   Pokémon code, data or art.
2. **Data on the card, streamed by area,** with a transition screen.
3. **On-screen controls,** one touch at a time.
4. **An Auto-play checkbox,** with the bot doubling as a host gate.
5. **Built in, in the Games menu,** with all heap freed on leaving.

**Open, for the owner:**

- **Q1 — how the data reaches the card.** Options:
  - `game.bin` is built by the owner on a computer with `mkgame.py` (it
    fetches the pinned Tuxemon) and copied to the card; the repo holds no art;
  - or the built file is committed under `courses/`-style data and installed
    on first run, which puts CC-BY-SA art into the repo (allowed, with credits).

  Recommendation: the first, at least until the allow-list is complete.
- **Q2 — the name** in the Games menu. Suggested: "Critters". "Tuxemon" is
  possible too, with credit, if the owner prefers it.
- **Q3 — the screen during Auto-play:** stay on (uses the battery) or dim as
  usual (the game keeps playing either way). Recommendation: stay on while the
  board is on USB power, dim as usual on battery.
- **Q4 — 4 or 8 bits per pixel** for the map view. **G0's answer: 8 bits**
  for map tiles (exact; 4 bits lost the encounter grass's colour) and 4 bits
  for battle sprites (no visible loss). `GAME_FORMAT.md` §4 has the renders'
  findings, and §9 here the heap it costs.
- **Q5 — when:** after the sync engine's open items (To Do, Address), or
  before.
- **Q6 — towns' buildings (new in G0).** Every Tuxemon town draws its houses
  from sheets that are partly under the XYG licence (§10), and the plan leaves
  XYG out. Options:
  - **find the XYG licence's text** (its site wasn't reachable) and decide
    whether it's compatible; if it is, the four sheets go on the allow-list;
  - **build towns from clean sheets only:** `core_city_and_country` (based
    on ArMM1998's public-domain "City and Country") is allowed and has two
    cottages, towers, a market stall and a castle gate, in a brighter style
    than the routes. The towns would be laid out in Tiled for this game,
    with Tuxemon's routes kept as they are;
  - **draw or source the buildings** from another CC0 or CC-BY set.

  Recommendation: the second, at least for the first region's two towns.
  It's map work in Tiled, not engine work, and it removes the question.
