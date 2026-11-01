# Dex: plan (proposal, 2026-11-01)

> **Status: design and plan only.** Nothing here is built. The owner asked
> (2026-11-01) for a new app in the Games folder, under Critters, named
> **Dex**. It works like the creature index in the monster-catching games:
> it lists every critter in the game, shows the ones the player has caught,
> and shows **???** for the rest. A character speaks through it, like Coach
> and Guru, and the owner will help draw that character's portrait. §9 has
> the decisions this plan needs from the owner.

The mock-ups are in `docs/img/dex/`. They're 240×320, the device's size,
drawn with the firmware's Palm fonts and the Critters screens' colours. The
script that drew them isn't product code and isn't in the repo.

| | | | |
|---|---|---|---|
| ![](img/dex/dex_1_folder.png) | ![](img/dex/dex_2_greet.png) | ![](img/dex/dex_3_list.png) | ![](img/dex/dex_4_caught.png) |
| 1. The Games folder | 2. Opening: the speaker | 3. The list | 4. A caught critter |
| ![](img/dex/dex_5_back.png) | ![](img/dex/dex_6_seen.png) | ![](img/dex/dex_7_never.png) | ![](img/dex/dex_8_where.png) |
| 5. Back view | 6. Seen, not caught | 7. Never seen | 8. Where: the speaker |

## 1) What it is

- A **separate app**, not a screen inside Critters. It opens from the Games
  folder at any time, whether or not a game is running, and reads two
  files: the game (`/sdcard/games/monster/game.bin`) and the last save
  (`save.dat`). It never writes either of them.
- **Every critter in the game file is listed**, numbered in Dex order
  (§4.2). Each has one of three states:
  - **caught**: everything is shown;
  - **seen** (met in battle, never owned): the sprite's silhouette, and ???
    for everything else (decision 2 in §9);
  - **never seen**: a question mark, and ??? for everything else.
- **The speaker** opens the app with a line about the player's progress, and
  answers **Where** on an entry. Like Coach and Guru, it has a portrait (60×63,
  the owner's art) and a speech bubble, and its lines are chosen by code from
  the player's state. It doesn't use a language model.

## 2) The screens

The Dex is red, the Critters red (the A button's 123, 28, 41), so it reads
as a device within the game's world. It keeps the system's frame: the black
title bar with Home and Menu, the Palm fonts, and full-screen pages like
Study and Guru (`ui_full_screen(1)`, so there's no Graffiti strip). The
casing's details come from the reference picture but are drawn our own
way: a lens and three lights along the top, a grey bezel with a clipped
corner, a cream screen, a green info panel, and blue keys.

1. **The Games folder** (mock-up 1). Dex is the seventh icon. The folder's
   grid is a wrapping flex row with even spacing, so a lone seventh icon
   sits **directly under Critters** without any layout code. The icon is a
   24×22 one-bit glyph like the others (`tools/gen_dex_icon.py`, as
   `gen_critters_icon.py`): a closed case with a lens.
2. **Opening** (mock-up 2). The list's top half, with the speaker below it.
   Tapping anywhere dismisses the bubble, as with Guru's greeting.
3. **The list** (mock-up 3).
   - The top strip shows the counts: **Seen 11  Caught 6 / 27**.
   - Each row shows the number, the critter's 24×24 icon, the name, and a
     mark: a filled capture ball for caught, a ring for seen.
   - A seen critter's icon is its silhouette, and its name is ???. A
     never-seen critter has no icon.
   - Rows are 22 px, so nine fit on a page. **Up** and **Down** turn pages,
     in Palm's way, with a Palm scroll bar showing the position. **Caught**
     filters to the caught critters. **Talk** brings the speaker back.
   - Tap a row to open it.
4. **An entry** (mock-ups 4 and 5).
   - The screen shows the front sprite at 2×, 128 px.
   - Beside it: the number, the name, the type chips, the next evolution
     ("Lv 24 to Agnidon"), and "caught".
   - The green panel shows the category ("FALSE DRAGON"), the height and
     weight, and the description.
   - The keys are ◀ and ▶, which step through the Dex, skipping nothing;
     **Back**, which shows the back sprite (its label becomes Front);
     and **Where**.
5. **Seen and never seen** (mock-ups 6 and 7). Everything is ??? except
   the number. A seen critter shows its silhouette, made at run time from the
   sprite's transparency (no new data). The keys stay, and none is dimmed
   (P4, `PALM_ARCHIVE_REVIEW.md`). Where on an unknown critter gets the
   speaker's line, "Nothing on this one yet."
6. **Where** (mock-up 8). The speaker covers the green panel and the keys
   and says where the critter lives. The answer depends on the act, and
   comes from the game's encounter tables (§4.3). Tap to close.

The capture ball and the casing are our drawings. No logo, name or artwork
from the reference picture or the games it shows is used.

## 3) The speaker

The owner will draw the portrait: **60×63**, the same as Coach, Guru and
Study, through `gen_faces.py --from-exact`, so the owner's pixels are used
as they are. Until the art arrives, the mock-ups show a dashed placeholder.

**Who it is** is decision 1 (§9). I recommend **Dr Wren**. The game
already has her: she runs the Lab in Larkhollow, gives the player the
first critter, and is the last keeper. Making her the voice of the Dex
ties the app to the story. Her lines can change after the player beats her
("The plate's yours, so this is just between keepers now").

The other option is a new character, perhaps the Lab helper who now only
says the type tip. That avoids the odd moment of the final opponent coaching
you.

**What she says.** The lines are chosen in `dex.c` (logic stays out of
`ui.c`) from the save and the game, with the same structure as
`st_advise()`.

| When | Example |
|---|---|
| No save yet | "No keeper's licence, no Dex. Start a game in Critters first." |
| Opening, early | "Six of twenty-seven. Something leafy keeps darting through Route 2 at dusk. Have a look." The hint names a critter **not yet seen** that the current act's tables can give, and the nearest area that has it. |
| Opening, a line not finished | "Your Vamporm's line has one left to find." |
| Opening, every one seen | "You've met them all. Catching them is another thing." |
| Opening, complete | "Twenty-seven of twenty-seven. I've nothing left to teach you." |
| Where, caught, wild | "Route 2's grass and the Old Keep, levels 12 to 22." |
| Where, keeper only | "No one's seen one wild. Ringwoman Juno keeps one; ask her." |
| Where, the player's starter | "Yours came from my lab." |
| Where, not caught | "Nothing on this one yet." |

About 25 lines in all. They're written in the firmware, like Guru's, because
they're the speaker's voice, not game data.

## 4) Data

### 4.1 What the save must remember (save version 4)

Today the save knows only the party and the box. A critter that was caught
and then evolved, released or traded no longer appears in either.

- **Two bitsets of 256 bits each** (64 B), *seen* and *caught*, keyed by
  the creature's **stable id** (`ids.tsv`), so that a rebuilt game file
  keeps the marks. This puts the save at version 4. Creature ids must stay
  under 256. The format's limit is written in `GAME_FORMAT.md` §5, and the
  builder refuses anything over it.
- `monster_play.c` sets the marks at these points:

  | Event | Seen | Caught |
  |---|---|---|
  | A wild encounter starts (`MP_EV_ENCOUNTER`) | ✓ | |
  | A keeper sends out a critter (`MP_EV_SENDS`) | ✓ | |
  | The player catches one (`MP_EV_CAUGHT`) | ✓ | ✓ |
  | The player receives a starter | ✓ | ✓ |
  | One of the player's critters evolves (`MP_EV_EVOLVE`) | ✓ | ✓ |

  This follows the games: evolving a critter registers its new form.
- **A version 3 save** loads with both sets made from the party and the
  box, so that a player's existing progress shows straight away.
- **The Dex reads `save.dat` only.** Opening any screen clears the content
  area, and that closes Critters and saves it (`content_clear()` calls
  `mon_ui_close()`). So by the time the Dex opens, the save is current, and
  the two never hold the game at once.

### 4.2 What the game file must add (format 2.2, minor)

A 2.1 reader skips both sections, so the game still plays on older
firmware. A 2.2 Dex shown an older file lists names and sprites and shows
"no Dex data in this file" in the panel.

- **`DEXE`, one 16 B record per creature, in `MONS` order:**
  - the Dex number (u16);
  - the height in cm (u16) and the weight in tenths of a kg (u16, so up to
    6,553.5 kg; Agnigon is 2,000);
  - the category (a `TEXT` reference);
  - the description (a `TEXT` reference);
  - 2 B reserved.

  That's 432 B, and the text adds about 2.5 KB.
- **`ICON`, the 24×24 list icons:** the sheets' first menu frame, 4-bit with
  its own 15-colour palette. That's 328 B a creature, about **8.9 KB** for 27.
  The sheets already have the frames; the builder doesn't take them yet.
- **The source.**
  - Height, weight and category come from Tuxemon's monster data.
  - The description comes from Tuxemon's English text where it has one. It
    has one for **13 of the 27**. **The other 14 need descriptions written for
    this project** (CC0, short and in the game's voice). I'll draft them,
    and the owner reviews them.
  - The **Dex order** is a `"dex"` list in `game_region.json`: the starters'
    lines first, then the rest in the order a player meets them. The owner
    controls that order, and new critters are added to it.
- **Both dumps** (`mkgame.py --dump`, `monster_test.c`) print the new
  sections. The round trip compares them byte for byte, as now.

**Cost: about 12 KB**, 2 % of the 572,000 B file. The partition has about
400 KB free.

### 4.3 Where: worked out, not stored

The **Where** line is computed when it's asked for:

- go through the areas' zones and the encounter tables they name, through
  the act's table steps (`META` 0x14, the save's flags);
- collect the areas and level ranges that give the critter;
- separately, list the keepers whose teams include it, and note whether it's
  the player's starter.

No new data is needed, and it's correct for whichever act the save is in.

## 5) Memory

The Dex obeys the same rule as the other screens: **one heap block, taken
when the app opens and freed when it closes.** It adds **no static DRAM**.

| What | Size |
|---|---|
| The open game file's directory and the save's header and bitsets | about 1 KB |
| One entry's sprite, decoded to RGB565 with alpha, drawn at 2× by `lv_image`'s scale (no second buffer) | 12 KB |
| The page's icons: nine at 24×24 RGB565 with alpha | 15.5 KB |
| The page's labels and rows: **nine rows reused** across pages rather than 27 objects | about 3 KB of the LVGL pool |
| **Total** | **about 30 KB of heap, 3 KB of the pool** |

There's about 106 KB of heap free with the SD card mounted, and 11 KB free
in the LVGL pool. Paging with Up and Down, rather than scrolling 27 rows,
keeps the Dex within the pool. The smoke tour's `B` and `E` commands prove
the heap is given back, as they do for Study.

## 6) Code

| File | What |
|---|---|
| `firmware/main/dex.c`, `dex.h` | New. Opening the game and the save read-only; the entry for a number; the Where search (§4.3); the speaker's lines (§3). No LVGL. |
| `firmware/main/ui.c` | A "Dex" section: the list, the entry, and the bubble, using `wk_`- and `st_`-style helpers. The seventh Games icon. |
| `firmware/main/monster.c`, `.h` | Reads `DEXE` and `ICON` (2.2). `mg_dex()` and `mg_icon()`. |
| `firmware/main/monster_play.c`, `.h` | The marks, save version 4, and loading a version 3 save. |
| `firmware/main/palm_icons.c` | `icon_dex`, from `tools/gen_dex_icon.py`. |
| `firmware/main/palm_icons.c` (where `gen_faces.py`'s portraits live) | The speaker's portrait, once the owner sends it. |
| `tools/mkgame.py` | Format 2.2: `DEXE` and `ICON`, the `"dex"` order, the description source, and the dump. |
| `tools/game_region.json` | The `"dex"` order and the 14 written descriptions. |
| `docs/GAME_FORMAT.md` | The two sections, the save at version 4, and the id limit. |

## 7) Gates

- **`make -C sim dex`** (new, in `ci.yml` and CLAUDE.md's list):
  - the marks are set by each event in §4.1;
  - a version 3 save upgrades correctly;
  - the Where search, for each act, matches a list worked out by hand from
    `game_region.json`;
  - the speaker's choice of line for each state;
  - Auto-play's full run ends with every critter it met marked seen and
    every critter it owned marked caught.
- **`make -C sim monster`:** the existing bot gate, with save version 4.
- **`tests/mkgame_test.py`:** `DEXE` and `ICON` on the fixture,
  byte-for-byte rebuilds, and a refusal for a creature id of 256 or more.
- **Smoke shots:**
  - `dex_folder`, `dex_greet`, `dex_list`, `dex_list_p2`;
  - `dex_entry_caught`, `dex_entry_back`, `dex_entry_seen`, `dex_entry_never`;
  - `dex_where`;
  - `dex_no_save`, the first run with no game played.

  A smoke command `D <seen-ids> <caught-ids>` will write a save with those
  marks, as the `F` command sets flags and coins.
- Every gate in CLAUDE.md before each commit, the firmware build, and the
  static DRAM checked unchanged.

## 8) Phases, one commit each

- **D1, the data.** Format 2.2 (`DEXE`, `ICON`, the order, the 14
  descriptions), save version 4 with the marks, `make -C sim dex` for the
  marks, and the dumps.
- **D2, the app.** `dex.c` and the screens, the Games icon, the smoke shots,
  and the Where search. The portrait is a placeholder until the art comes.
- **D3, the speaker.** The owner's portrait through `gen_faces.py
  --from-exact`, the lines, and their gate.
- **D4, the bench.** The owner's `[d]` items: the Dex opens from the folder;
  a caught critter shows in full after a save; a v3 save keeps its party in
  the Dex; the heap is unchanged after closing; ◀ ▶ are fast enough on the
  board.

Separately, and not part of the Dex: **the owner's new critters.** Built
2026-11-01 (`tools/game_creatures/README.md`): `tools/mksheet.py` turns a
drawing into its sheet, and a creature of our own has its rules in
`tools/game_creatures/<slug>.yaml` and an allow-list line with status
"ours". The first are the fire fox line (Cinderkit, Scorchvix, Pyrewing) and
the axolotl line (Gillpip, Rillotl, Maelstrotl), so the game has **33**
critters now, not the 27 counted below; their
descriptions are in their rules, ready for the Dex. Who uses them in Acts 2
and 3 is still open: today they're only wild.

## 9) Decisions for the owner

1. **Who speaks?** Dr Wren (recommended, §3) or a new character. Either
   way, the owner draws the 60×63 portrait.
2. **A critter seen but not caught:** show its silhouette, with ??? for
   everything else (recommended, as in the games), or show nothing but ???,
   exactly as for one never seen (the brief as written).
3. **The list icons:** add the 24×24 icons (about 9 KB in the file,
   recommended), or a text-only list.
4. **The Dex order:** the starters' lines first, then in the order a player
   meets them (recommended), or Tuxemon's numbering.
5. **The 14 missing descriptions:** I'll draft them for review
   (recommended), or the owner writes them.
6. **The format change:** game format 2.2 and save version 4, both
   additions that older firmware handles: it skips the new sections, and
   an older save is upgraded.
