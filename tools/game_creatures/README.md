# Critters of our own

Critters drawn for this project, beside the ones `mkgame.py` takes from
Tuxemon. Each one is three files and an allow-list line:

| File | What it is |
|---|---|
| `tools/game_art/src/<slug>.jpg` (or `.png`) | The drawing as it came: front, back and two icons, blown up, on white or a checkerboard (light or dark). Kept so the sheet can always be made again. |
| `tools/game_art/<slug>-sheet.png` | The 128 × 88 battle sheet the game reads, made from the drawing by `tools/mksheet.py`. |
| `tools/game_creatures/<slug>.yaml` | Its rules: name, types, body shape, stage, moves by level, evolution, catch rate, and the Dex text. |
| `tools/game_allow.tsv` | `creature <slug> <licence> <authors> tools/game_art/<slug>-sheet.png ours` |

## Adding one

1. **Save the drawing** as `tools/game_art/src/<slug>.jpg`. The slug is the
   name in lower case (`cinderkit`). Lay it out like a Tuxemon sheet: the
   front top left, the back top right, the two 24 px icons under the front.
   It may be any size and needn't sit on an exact grid. The background may be
   white or a checkerboard, light or dark: its greys are learnt from the
   picture's border. A splash or tornado may come close to the other drawing:
   each loose piece goes with the drawing nearest it. The icons may be left
   out; the front is then shrunk to make them.
2. **Make the sheet:**

       python3 tools/mksheet.py tools/game_art/src/<slug>.jpg \
           -o tools/game_art/<slug>-sheet.png --preview /tmp/<slug>.png

   It prints what it found: the drawing's pixel size, where each part was,
   how much it was shrunk (a drawing made at twice the game's size is halved),
   and the colours. Look at the preview: the drawing beside the sheet at 4×.
   The front and back share **15 colours**, as every critter in the game
   does, so a drawing with many shades loses some. A drawing already at
   128 × 88 with 15 colours or fewer goes in exactly as drawn.

   **Then fix the eyes.** Halving makes a pupil swell and loses its highlight,
   so the sheet's eyes are drawn by hand, from
   `tools/game_art/src/<slug>.eyes.json`. The first build writes that file as a
   draft from the eyes it can find, and tells you what it couldn't find. It
   finds round eyes well, and misses or misplaces narrow and slanted ones, so
   always look:

       python3 tools/mksheet.py tools/game_art/src/<slug>.jpg --look /tmp/<slug>.png

   That's each part at 10× with a ruler, in the file's own coordinates (x, y
   of the part, cropped as it sits in the sheet). Each eye in the file:

       {"wipe": [x0, y0, x1, y1], "at": [x, y], "draw": [".DD.", "DHDD", "DDDD", ".DD."]}

   - `wipe`: the dark pixels in this box are painted over with the colour
     around them, except any beside the clear, which are outline;
   - `at` and `draw`: then this is drawn: `D` is the part's darkest colour,
     `H` its lightest, a dot leaves the pixel as it is. An eye may also have
     `"colors": {"Y": [253, 182, 31]}`, which makes `Y` the part's nearest
     colour to that. Slanted eyes do well in a bright colour under a brow, as
     Scorchvix's and Pyrewing's fronts do: `["...D", ".DDD", "DYYD"]`.

   The front is the one that shows on the device, along with the back:
   `mkgame.py` doesn't read a sheet's icons today.

   An empty list means that part's eyes are fine as they came (Maelstrotl's
   front). Edit the file and build again; each try takes a few seconds. The
   file is part of the drawing: `--check` refuses a drawing without one.
3. **Write its rules** in `tools/game_creatures/<slug>.yaml`, copying one of
   the fire fox line's. The keys:
   - `name`, `description` (the Dex's text), `drawing` (step 1's path);
   - `types`: one or two of Tuxemon's (cosmic, earth, fire, frost, heroic,
     lightning, metal, normal, shadow, sky, venom, water, wood);
   - `shape`: Tuxemon's body shapes set the stats (blob, brute, dragon, flier,
     grub, humanoid, hunter, landrace, leviathan, piscine, polliwog, serpent,
     sprite, varmint);
   - `stage`: basic, stage1, stage2 or standalone;
   - `moveset`: Tuxemon's techniques by level. A move whose effects the game
     doesn't play is left out (the build report lists them; `mkgame.py
     --report`), so give it at least one usable move at level 1;
   - `evolutions`: `at_level` and `monster_slug`, which must be on the
     allow-list too;
   - `catch_rate`, `lower_catch_resistance`, `upper_catch_resistance`,
     `experience_group` (default, fast or slow), as in Tuxemon.
4. **Add the allow-list line** (status `ours`), with the licence and who drew
   it. The credits screen shows it.
5. **Put it somewhere:** an encounter table, a keeper's team, or the
   starters, in `tools/game_region.json`.
6. **Build and check:**

       python3 tools/mkgame.py --strict -o courses/critters/game.bin
       python3 tests/mkgame_test.py

   The build refuses a bad type, shape, stage or key, a move Tuxemon doesn't
   have, an evolution into a critter that isn't listed, and a missing sheet.
   The test checks that each sheet is what its drawing makes, to the byte,
   and that the credits name it. Then run the rest of the gates (CLAUDE.md),
   `make -C sim monster` above all: Auto-play must still finish the game.

## The fire fox line (2026-11-01)

The owner's first critters: **Cinderkit** (fire, varmint) evolves at 16 into
**Scorchvix** (fire, hunter), which evolves at 32 into **Pyrewing** (fire and
sky, brute). The moves are Tuxemon's fire moves, with Wing Tip and Peregrine
for Pyrewing. They live in the wild, rarely:

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Cinderkit | Cotton Tunnel, 6–8 | Route 1, 12–15 | |
| Scorchvix | | Cotton Tunnel, 17–20 | Route 2, 26–30; Dragon's Cave, 30–32 |
| Pyrewing | | | Dragon's Cave once Wyrmkeeper Ashe is beaten, 36–38 |

The drawings were made at twice the game's size, so the sheets are halved.

## The axolotl line (2026-11-01)

The owner's second line, on a dark checkerboard: **Gillpip** (water,
polliwog) evolves at 16 into **Rillotl** (water, humanoid), which evolves at
32 into **Maelstrotl** (water, brute). The moves are Tuxemon's water moves
(Bubbles, Water Bullet, Font, Water Blast, Kraken, Tsunami) with Gnaw, Fluff
Up, Saber and All In. Hook, Blood Bond and Wallow would suit them, but the
game doesn't play their effects yet. They live in the wild, rarely:

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Gillpip | Brideswood, 5–7 | Brideswood, 14–17 | |
| Rillotl | | | Route 1, 22–26; Brideswood, 24–28 |
| Maelstrotl | | | The Old Keep once Wyrmkeeper Ashe is beaten, 34–36 |

These drawings were at twice the game's size too, so the sheets are halved.
Maelstrotl is the busiest drawing yet; at 15 colours and half size its
spines and armour come out grainy.
