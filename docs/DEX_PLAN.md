# Dex: plan (proposal, 2026-11-01; redesigned 2026-11-02)

> **Status: design and plan only.** Nothing here is built. The owner asked
> (2026-11-01) for a new app in the Games folder, under Critters, named
> **Dex**. It works like the creature index in the monster-catching games:
> it lists every critter in the game, shows the ones the player has caught,
> and shows **???** for the rest.
>
> **The owner's redesign (2026-11-02):** the Dex is a **hardware gadget**. Its
> bezel and its keys never move or change, on any screen, and the title bar is
> part of them. All text is inside a subscreen, except what's printed on the
> hardware: the key labels and the name. The speaker comes through the
> subscreens too, so the speech bubble and portrait overlay of Coach and Guru
> **aren't used**. The owner supplied the speaker's art. §9 has the decisions,
> all made.

The mock-ups are in `docs/img/dex/`. They're 240×320, the device's size,
drawn by `tools/mock_dex.py` (a design aid, not product code) with the
firmware's Palm fonts and the Critters screens' colours. Mock-up 1 is from the
first design and is still right.

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
- **A fixed gadget.** One bezel and one set of four keys, used by every screen.
  Only what's inside the two subscreens changes.
- **Every critter in the game file is listed**, numbered in Dex order
  (§4.2). Each has one of three states:
  - **caught**: everything is shown;
  - **seen** (met in battle, never owned): the sprite's silhouette, and ???
    for everything else;
  - **never seen**: a question mark, and ??? for everything else.
- **The speaker** opens the app with a line about the player's progress, and
  answers **Talk** on an entry. His lines are chosen by code from the player's
  state. It doesn't use a language model.

## 2) The gadget

The Dex is red, the Critters red (the A button's 123, 28, 41), so it reads
as a device within the game's world. Its details come from the reference
picture but are drawn our own way. It's a full-screen page like Study and Guru
(`ui_full_screen(1)`, so there's no Graffiti strip).

### 2.1 What never changes

| Part | Where (x, y) | Notes |
|---|---|---|
| **The title bar, as hardware** | 0–239, 0–23 | Same layout and dimensions as the system's; see below |
| The top strip: a lens and three lights | 0–239, 24–59 | No text. "No. 001" and the counts moved into the subscreens |
| The upper bezel, with one red light | 6–233, 60–206 | Grey, with clipped corners |
| **The upper screen**: cream, in colour | 15–224, 66–193 (**210×128**) | |
| **The lower screen**: green, in one ink colour | 7–232, 213–285 (**226×73**) | Five lines of text |
| **The four keys** | y 292–314 | **◀ List Talk ▶**, always these labels |

**Why the upper screen is 128 high.** In the first mock-ups it was 121. A
front or back sprite is drawn at 2×, and 8 of the 36 critters (measured before the wood and venom lines) are over 60 px
tall, up to 64 (`pigabyte`, `selmatek`, `dracune`, `propellercat`, `arthrobolt`,
`cateye`, `noctalo`, `djinnbo`; measured from their sheets). They'd lose up to
7 rows, so the grey bezel gives up that height. None is over 64 px wide, so a
2× sprite always fits the 128 px slot.

**The title bar** keeps every part where the system's puts it, so it stays
familiar, but it's drawn as part of the case:

| Part | Where (x, y) | As hardware |
|---|---|---|
| The name, Dex | From x 4 | Printed on the red case, in cream |
| The clock | Its usual centred place | Dark ink in a small green screen, x 72–166, y 2–21, sized for the widest clock so it doesn't change with the time |
| Home | 169–211, 1–20 | A blue key, with its usual icon in white |
| Menu | 214–238, 1–20 | A blue key, with its usual icon in white |

They do what they do everywhere else. Home leaves the app, and Menu opens the
system's menu. The menu and any system alert (a reminder, say) still appear over
the gadget while they're open, as they do over any app, and leave it unchanged.

The bezel is built **once**, when the app opens, and stays until it closes.
Moving between screens only refills the subscreens. `title_bar` belongs to the
whole system, so `content_clear()` must put it back when the Dex closes.

### 2.2 The keys

This set, with Home and Menu, is the gadget's whole keyboard. The labels never
change, and no key is ever dimmed or hidden (P4,
`PALM_ARCHIVE_REVIEW.md`). A key inverts while it's held, like Critters' A
button. What each one does depends on what the screens are showing:

| Key | The list | An entry | The speaker talking |
|---|---|---|---|
| **◀ ▶** | Previous or next page | Previous or next critter, skipping nothing | He stops, and the key acts on what's beneath |
| **List** | All critters or caught only, in turn | Back to the list, at this critter's page | He stops, and shows the list |
| **Talk** | His line about the player's progress | His line about where this critter lives | He stops |

The upper screen also takes taps, for what's shown there: a row opens that
critter, and a sprite turns it to its back and front again. Tapping either
subscreen while he's talking stops him, as with Guru's greeting.

This replaces the first design's two key sets (**Up Down Caught Talk** and
**◀ Back Where ▶**), where the labels changed from screen to screen. "Back"
was also a confusing label, since it looks like a way out.

### 2.3 The screens

1. **The Games folder** (mock-up 1). Dex is the seventh icon. The folder's
   grid is a wrapping flex row with even spacing, so a lone seventh icon
   sits **directly under Critters** without any layout code. The icon is a
   24×22 one-bit glyph like the others (`tools/gen_dex_icon.py`, as
   `gen_critters_icon.py`): a closed case with a lens.
2. **Opening** (mock-up 2). The speaker's line about the player's progress, with
   the counts on his card. The list is beneath it.
3. **The list** (mock-up 3).
   - **Upper screen:** rows of 24 px, the icon's height, so **five fit on a
     page**. Each shows the number, the critter's 24×24 icon, the name, and a
     mark: a filled capture ball for caught, a ring for seen. A Palm scroll bar
     shows the position.
   - A seen critter's icon is its silhouette, and its name is ???. A
     never-seen critter has no icon.
   - **Lower screen:** the counts (**SEEN 11  CAUGHT 6 / 42**), the filter and
     page ("Showing all, page 1 of 7"), and what to do next.
4. **An entry** (mock-ups 4 and 5).
   - **Upper screen:** the sprite at 2×, and beside it the number, the name,
     the type chips, the next evolution ("Lv 24 to Agnidon"), "caught", and
     which side is showing (Front or Back).
   - **Lower screen:** the category ("FALSE DRAGON"), the height and weight,
     and the description.
5. **Seen and never seen** (mock-ups 6 and 7). Everything is ??? except
   the number. A seen critter shows its silhouette, made at run time from the
   sprite's transparency (no new data). The keys behave as on any entry. Talk
   on an unknown critter gets "Nothing on this one yet."
6. **Where** (mock-up 8). Talk on an entry: the speaker says where the
   critter lives. The answer depends on the act, and comes from the game's
   encounter tables (§4.3).

**The description and his line use the same lower screen,** so a description
must fit in the three lines left under the category and the height and weight.
His lines get all five.

The capture ball and the casing are our drawings. No logo, name or artwork
from the reference picture or the games it shows is used.

## 3) The speaker

**The art** is the owner's, kept as `docs/img/dex/speaker/source.jpg`: a
dark-skinned man with a moustache in a khaki field vest, with badges and a
camera. It's pixel art on a grid of about 9.9 px in a 1024 px picture, so it's
really **102×102**. `docs/img/dex/speaker/colour_102.png` is that grid sampled
back to its pixels, with the backdrop set to the screen's cream and 24 colours
left. **It's in colour by the owner's choice:** a one-ink portrait can't show a
character who isn't light-skinned. It hasn't been edited by hand, and from now
on that PNG is the art: edit it directly, and don't sample it from the JPEG again.

**His place, whenever he's talking** (mock-ups 2 and 8):

- **Upper screen:** he stands **against the right side**, at 1×, 102×102, on
  the screen's bottom edge. What he's talking about is on a **card to his left,
  like a news report's**: 103×122, with a red heading band.
  - Opening: **YOUR DEX**, with the caught and seen counts.
  - On an entry: **FIELD REPORT**, with the critter's sprite at 1× and a caption
    band under it with its name and number. A seen critter shows its silhouette
    and ???, and a never-seen one a question mark and ???.
- **Lower screen:** his words, full width. That's about 38 characters by 5 lines.

The speech bubble and portrait overlay of the other apps aren't used. They sit
on top of the page, and nothing of the app's own may sit over this bezel or its
keys.

**Who he is.** He replaces the earlier proposal of Dr Wren, so the player no
longer gets coached by the game's final opponent. **His name is never shown**,
so none is needed, and the card and his lines don't refer to one.

**What he says.** The lines are chosen in `dex.c` (logic stays out of
`ui.c`) from the save and the game, with the same structure as
`st_advise()`. Each must fit in the lower screen (§7).

| When | Example |
|---|---|
| No save yet | "No keeper's licence, no Dex. Start a game in Critters first." |
| Opening, early | "Six of thirty-three. Something leafy keeps darting through Route 2 at dusk. Have a look." The hint names a critter **not yet seen** that the current act's tables can give, and the nearest area that has it. |
| Opening, a line not finished | "Your Vamporm's line has one left to find." |
| Opening, every one seen | "You've met them all. Catching them is another thing." |
| Opening, complete | "Thirty-three of thirty-three. I've nothing left to teach you." |
| Talk, caught, wild | "Route 2's grass and the Old Keep, levels 12 to 22." |
| Talk, keeper only | "No one's seen one wild. Ringwoman Juno keeps one; ask her." |
| Talk, the player's starter | "Yours came from Dr Wren's lab." |
| Talk, not caught | "Nothing on this one yet." |

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
| The page's icons: five at 24×24 RGB565 with alpha | 8.6 KB |
| The fixed bezel and keys, and the page's labels and rows: **five rows reused** across pages rather than 42 objects | about 3 KB of the LVGL pool (estimate) |
| The speaker's portrait: RGB565 with no alpha, 20.8 KB of flash, drawn from there | none intended, to be measured in D3 |
| **Total** | **about 22 KB of heap, 3 KB of the pool** |

There's about 106 KB of heap free with the SD card mounted, and 11 KB free
in the LVGL pool. Paging with ◀ and ▶, rather than scrolling 42 rows,
keeps the Dex within the pool. The smoke tour's `B` and `E` commands prove
the heap is given back, as they do for Study.

## 6) Code

| File | What |
|---|---|
| `firmware/main/dex.c`, `dex.h` | New. Opening the game and the save read-only; the entry for a number; the Where search (§4.3); the speaker's lines (§3). No LVGL. |
| `firmware/main/ui.c` | A "Dex" section: the hardware title bar (restyled on opening, put back by `content_clear()`, with `mk_tb_silk` taking the key colour), the bezel and keys built once, and functions that fill the two subscreens for the list, the entry and the speaker. It doesn't use the speech-bubble code. The seventh Games icon. |
| `firmware/main/monster.c`, `.h` | Reads `DEXE` and `ICON` (2.2). `mg_dex()` and `mg_icon()`. |
| `firmware/main/monster_play.c`, `.h` | The marks, save version 4, and loading a version 3 save. |
| `firmware/main/palm_icons.c` | `icon_dex`, from `tools/gen_dex_icon.py`. |
| `firmware/main/palm_icons.c` | The speaker's portrait: an RGB565 image from a small generator, `tools/gen_dex_speaker.py`, which reads `colour_102.png` 1:1 and is checked in the `dex` gate. |
| `tools/mock_dex.py` | Draws the mock-ups. Not product code. |
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
  - **every line fits**: each line and each worked-out Where answer wraps to at
    most five lines of the lower screen in the Palm font, and every description
    to at most three;
  - Auto-play's full run ends with every critter it met marked seen and
    every critter it owned marked caught.
- **`make -C sim monster`:** the existing bot gate, with save version 4.
- **`tests/mkgame_test.py`:** `DEXE` and `ICON` on the fixture,
  byte-for-byte rebuilds, and a refusal for a creature id of 256 or more.
- **Smoke shots:**
  - `dex_folder`, `dex_greet`, `dex_list`, `dex_list_p2`, `dex_list_caught`;
  - `dex_entry_caught`, `dex_entry_back`, `dex_entry_seen`, `dex_entry_never`;
  - `dex_where`;
  - `dex_tall`, a critter 64 px tall, to see that it isn't clipped;
  - `dex_no_save`, the first run with no game played.

  Comparing the shots should show the bezel, title bar and keys
  **pixel-identical** on all of them: `make -C sim dex` compares those regions
  (bar the clock's digits and a key held down). A further shot, `dex_left`, leaves
  for another app, and must show the system's own black title bar again.

  A smoke command `D <seen-ids> <caught-ids>` will write a save with those
  marks, as the `F` command sets flags and coins.
- Every gate in CLAUDE.md before each commit, the firmware build, and the
  static DRAM checked unchanged.

## 8) Phases, one commit each

- **D1, the data.** Format 2.2 (`DEXE`, `ICON`, the order, the 14
  descriptions), save version 4 with the marks, `make -C sim dex` for the
  marks, and the dumps.
- **D2, the app.** The fixed bezel and keys, `dex.c` and the screens, the Games
  icon, the smoke shots, and the Where search. His lines are placeholders.
- **D3, the speaker.** The portrait and the news card, the lines, their fit gate,
  and the measured heap.
- **D4, the bench.** The owner's `[d]` items: the Dex opens from the folder;
  a caught critter shows in full after a save; a v3 save keeps its party in
  the Dex; the heap is unchanged after closing; ◀ ▶ are fast enough on the
  board; the keys are easy to hit; the portrait and the green clock read well on
  the panel.

Separately, and not part of the Dex: **the owner's new critters.** Built
2026-11-01 (`tools/game_creatures/README.md`): `tools/mksheet.py` turns a
drawing into its sheet, and a creature of our own has its rules in
`tools/game_creatures/<slug>.yaml` and an allow-list line with status
"ours". The first are the fire fox line (Cinderkit, Scorchvix, Pyrewing) and
the axolotl line (Gillpip, Rillotl, Maelstrotl), then the lightning line
(Sparkip, Nimbolt, Fulminax), and the wood and venom lines (Sprigling, Wildbough, Grovarch; Skullick, Ossicoil, Banewyrm), so the game has **42** critters now, not the 27 counted below; their
descriptions are in their rules, ready for the Dex. Who uses them in Acts 2
and 3 is still open: today they're only wild.

## 9) Decisions

**Decided by the owner, 2026-11-02:**

- The bezel and keys are fixed, all other text is in the subscreens, and the
  bubble overlay isn't used.
- **The speaker:** in colour in the upper screen, against its right side, with
  the content in a news-report card to his left, and his words in the lower
  screen (§3). He has no displayed name. The art is the owner's latest variant.
- **The title bar** is redrawn as part of the hardware, in the same layout and
  dimensions (§2.1).
- **The keys:** ◀ List Talk ▶, so caught-only is a second press of List and
  turning a sprite is a tap (§2.2).

**Taken on the recommendations, the owner's word, 2026-11-02:**

- **A critter seen but not caught** shows its silhouette, with ??? for everything
  else, as in the games.
- **The list has the 24×24 icons**, about 14 KB in the file for 42 critters.
- **The Dex order:** the starters' lines first, then in the order a player meets
  them.
- **The missing descriptions:** I draft them for the owner's review. They must fit
  three lines of the lower screen.
- **The format change:** game format 2.2 and save version 4, both additions that
  older firmware handles: it skips the new sections, and an older save is
  upgraded.

Nothing is open. Building starts at D1 (§8) when the owner asks for it.
