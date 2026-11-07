# Critters of our own

Critters drawn for this project, beside the ones `mkgame.py` takes from
Tuxemon. Each one is three files and an allow-list line:

| File | What it is |
|---|---|
| `tools/game_art/src/<slug>.jpg` (or `.png`) | The drawing as it came: front, back and two icons, blown up, on white, a checkerboard (light or dark) or transparent. Kept so the sheet can always be made again. |
| `tools/game_art/<slug>-sheet.png` | The 128 × 88 battle sheet the game reads, made from the drawing by `tools/mksheet.py`. |
| `tools/game_creatures/<slug>.yaml` | Its rules: name, types, body shape, stage, moves by level, evolution, catch rate, and the Dex text. |
| `tools/game_allow.tsv` | `creature <slug> <licence> <authors> tools/game_art/<slug>-sheet.png ours` |

## Adding one

1. **Save the drawing** as `tools/game_art/src/<slug>.jpg`. The slug is the
   name in lower case (`cinderkit`). Lay it out like a Tuxemon sheet: the
   front top left, the back top right, the two 24 px icons under the front.
   It may be any size and needn't sit on an exact grid. The background may be
   white or a checkerboard, light or dark: its greys are learnt from the
   picture's border. Or it may be transparent, which is best: nothing is
   guessed from the colours, so white fur on the edge stays. A splash or tornado may come close to the other drawing:
   each loose piece goes with the drawing nearest it. The icons may be left
   out; the front is then shrunk to make them.
2. **Make the sheet:**

       python3 tools/mksheet.py tools/game_art/src/<slug>.jpg \
           -o tools/game_art/<slug>-sheet.png --preview /tmp/<slug>.png

   It prints what it found: the drawing's pixel size, where each part was,
   how much it was shrunk (a drawing made at twice the game's size is halved),
   and the colours. Look at the preview: the drawing beside the sheet at 4×.
   The tool takes the smallest scale at which everything fits, icons included,
   so big icons can shrink the front, and a line's stages can come out in the
   wrong order of size. Put `"scale": N` at the top of the eyes file (below) to
   set the front and back at 1/N; the icons then fit themselves. Compare the
   sizes with the other lines'. A row or two over the 58 a front may stand
   loses its emptier edge. If a fine drawing at 1/2 comes out nearly black,
   `"ink": 2` beside the scale keeps outline only where it fills half a block,
   not a quarter. If light armour or a white ruff inside an outline comes out
   see-through, the tool took it for a gap between an arm and the body: put
   `"mirror": ["back"]` (or `"front"`) flips that part last: in battle a front faces left and a back right.
   `"holes": [[x, y], ...]` in the eyes file, a point in the picture inside
   each real gap, `[]` if there are none. Everything else enclosed then stays.
   If a gem or a glowing eye of a few pixels comes out as a big area's shade
   (the heroic line's red gems went brown), `"keep": [[r, g, b], ...]` gives
   each colour named its own place among the 15, front, back and icons alike;
   draw the gem again as an eye (below) in that colour. If the icons are drawn
   close under the front's feet and the tool takes them for part of it (it
   says "no icons drawn" though there are), `"icon_row": y` names the picture's
   y where the icons start: nothing below it joins anything above it.

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
   - `name`, `description` (the Dex's text: `mkgame.py` refuses more than three
     lines of its lower screen), `category`, `height_cm` and `weight_kg` (the Dex's
     too, and all required), `drawing` (step 1's path);
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

## The lightning line (2026-11-02)

The owner's third line, on real transparency: **Sparkip** (lightning, varmint)
evolves at 16 into **Nimbolt** (lightning, hunter), which evolves at 32 into
**Fulminax** (lightning, brute). The names, types, moves, places and Dex text
are ours for the owner to change. They live in the wild, rarely:

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Sparkip | Route 1, 3–5 | Route 2, 14–17 | |
| Nimbolt | | The Old Keep, 19–22 | Cotton Tunnel, 26–30 |
| Fulminax | | | The Old Keep once Wyrmkeeper Ashe is beaten, 34–36 |

These drawings are much finer than the first two lines', with no clean grid,
so the tool samples them every 3 picture pixels and shrinks by 1/5 (Sparkip)
and 1/4 (Nimbolt, Fulminax), set in their eyes files. That keeps their widths
(30, 45, 58) with the other lines' (29–34, 37–44, 49–56).

**Their lightning moves are few.** The game plays two of Tuxemon's lightning
attacks that deal plain damage: Lightning Spheres and Battery Discharge. Buzz,
Thunderball, Thunderclap, Surge, Laser Beam, Energy Claws, Energy Field and
Static Field all need effects the game doesn't play yet (flinching, exhausted,
blinded, sniping, charging), so the line's stronger attacks are Whirlwind,
Saber, Radiance, All In and Energy Beam. Battery Discharge, at 1.5, is their
strongest lightning move.

## The wood line (2026-11-02)

The owner's fourth line: **Sprigling** (wood, sprite) evolves at 16 into
**Wildbough** (wood, humanoid), which evolves at 32 into **Grovarch** (wood,
brute). As with the venom line, the names, moves, places and Dex text are ours
for the owner to change. Moves: Stick, Spiky Strike, Staff Smash, Blossom and
Woodsmash, with Gnaw, Fluff Up, Saber and All In. Cutting Leaves, Pollen Blast,
Splinter and Thorn Burst need effects the game doesn't play yet.

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Sprigling | Brideswood, 5–7 | Brideswood, 14–17 | |
| Wildbough | | Route 2, 17–20 | Brideswood, 24–28 |
| Grovarch | | | The Old Keep once Wyrmkeeper Ashe is beaten, 34–36 |

Scales 1/5, 1/4 and 1/2. Grovarch is a JPEG on a checkerboard with a real
5 px grid, set `"ink": 2`; its left eye was lost, so both are drawn in green.

## The venom line (2026-11-02)

The owner's fifth line: **Skullick** (venom, varmint) evolves at 16 into
**Ossicoil** (venom, serpent), which evolves at 32 into **Banewyrm** (venom,
dragon). Wings and all, Banewyrm is venom only, for the owner to change. Moves:
Spit Poison, Acid, Sting, Muck, Caustic Spray and Viper, with Gnaw, Saber and
Wing Tip.

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Skullick | Cotton Tunnel, 6–8 | Route 1, 12–15 | |
| Ossicoil | | Cotton Tunnel, 17–20 | The Old Keep, 28–32 |
| Banewyrm | | | Dragon's Cave once Wyrmkeeper Ashe is beaten, 36–38 |

Scales 1/5, 1/5 and 1/2. Banewyrm (a JPEG on white, `"ink": 2`) is the busiest
drawing yet and the largest, 58 × 58; it's grainy on the torso. Ossicoil's and
Banewyrm's fronts have magenta eyes drawn in a dark slit.

## The sky line (2026-11-03)

The owner's sixth line: **Cirrup** (sky, varmint) evolves at 16 into **Cumulynx**
(sky, hunter), which evolves at 32 into **Tempyre** (sky, dragon). Sky only,
storm wings and all, for the owner to change, as are the names, moves, places
and Dex text. Moves: Gust, Peck, Whirlwind, Wing Tip, Fledgling and Peregrine,
with Gnaw, Fluff Up, Battery Discharge, Saber and All In.

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Cirrup | Route 1, 3–5 | Route 1, 12–15 | |
| Cumulynx | | The Old Keep, 19–22 | Route 1, 22–26 |
| Tempyre | | | Dragon's Cave once Wyrmkeeper Ashe is beaten, 36–38 |

All at 1/2: 38 × 32, 53 × 53, 57 × 56. Their white ruffs needed `"holes"`.
Cirrup's eyes and Tempyre's three gold ones are drawn by hand; Cumulynx's came
through. Both big ones are grainy on the wings.

## Rivetron (2026-11-03)

**Rivetron** (metal, humanoid) is one critter on its own, `stage: basic` with no
evolution, all ours to change but the drawing. Moves: Slice, Blade, Bullet,
Perfect Cut, Saber, Steel Jaws, Needle and Adamantine. It lives in Cotton Tunnel,
17–20, and The Old Keep, 28–32. 1/2 with `"ink": 2`, 35 × 48. Its back is cut
off at the waist, as drawn.

## The heroic line (2026-11-05)

The owner's seventh line, on real transparency: **Valkit** (heroic, varmint)
evolves at 16 into **Valorix** (heroic, hunter), which evolves at 32 into
**Paladrax** (heroic, brute). The names, types, moves, places and Dex text are
ours for the owner to change. Moves: Lantern, Shadow Boxing, Punch, Radiance
and All In, with Gnaw, Fluff Up, Assault and Adamantine. One Two, Sudden Glow,
Hook, Lock Horns, Invictus and Berserk need effects the game doesn't play yet.

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Valkit | Route 1, 3–5 | Route 2, 14–17 | |
| Valorix | | The Old Keep, 19–22 | Cotton Tunnel, 26–30 |
| Paladrax | | | The Old Keep once Wyrmkeeper Ashe is beaten, 34–36 |

All three are in the Reach too, as every line is. Scales 1/5, 1/5 and 1/4
(30 × 34, 41 × 45, 52 × 58), the backs mirrored; Valorix and Paladrax are fine
drawings, set `"ink": 2`. The red gem on each forehead (and Paladrax's chest)
went brown in the 15 colours: `"keep"` holds the red, and the gems are drawn
again by hand, as are all the gold eyes.

## The cosmic line (2026-11-05)

The owner's eighth line, on real transparency: **Starlet** (cosmic, sprite)
evolves at 16 into **Nebulet** (cosmic, sprite), which evolves at 32 into
**Astrolune** (cosmic, humanoid). Ours to change but the drawings, as above.
Moves: Crystal, Lineage, Ruby and Dreamwalk, with Gnaw, Phantasmal Force, Fluff
Up, Pyrokinesis and Energy Beam. Arcane Eye, Starfall, Mind Vise, Counterspell,
Cloud Aether, Divinity Beam, Sands of Time and Demiurge need effects the game
doesn't play yet.

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Starlet | Route 1, 3–5 | Route 1, 12–15 | |
| Nebulet | | The Old Keep, 19–22 | Route 1, 22–26 |
| Astrolune | | | Dragon's Cave once Wyrmkeeper Ashe is beaten, 36–38 |

Scales 1/6, 1/5 and 1/5 (30 × 38, 39 × 49, 48 × 50); Starlet's back is
mirrored, and Nebulet and Astrolune are `"ink": 2`. Nebulet's and Astrolune's
icons sit close under their feet, so they need `"icon_row": 755`. The cyan
eyes and forehead stars of all three are drawn by hand; the icons' closed eyes
came through. Nebulet and Astrolune are dark, busy drawings and come out
grainy, Astrolune most.

## The crystal line (2026-11-05)

The owner's ninth line, a JPEG on a drawn checkerboard: **Geodot** (earth,
blob) evolves at 16 into **Quartzor** (earth, brute), which evolves at 32 into
**Prismyth** (earth and cosmic, humanoid). Ours to change but the drawings.
Moves: Hammerhead, Rock, Grinding, Greenstone and Earthquake, with Gnaw,
Crystal, Ruby, Fluff Up and Adamantine. They take the venom line's places:

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Geodot | Cotton Tunnel, 6–8 | Route 1, 12–15 | |
| Quartzor | | Cotton Tunnel, 17–20 | The Old Keep, 28–32 |
| Prismyth | | | Dragon's Cave once Wyrmkeeper Ashe is beaten, 36–38 |

Scales 1/4, 1/3 and 1/3 (32 × 30, 48 × 50, 53 × 50), all `"holes": []`: the
tool took light crystals and Geodot's gold gem for see-through gaps. The
pink eyes needed `"keep"` (and Prismyth's gold one too, with `"ink": 2`).
Geodot's two-coloured eyes are drawn by hand on the front and icons; Quartzor's
icons and the brow of its cyan eye too.

## The rock line (2026-11-05)

The owner's tenth line, JPEGs on a drawn checkerboard: **Pebblub** (earth,
blob) evolves at 16 into **Bouldrom** (earth, brute), which evolves at 32 into
**Tectorath** (earth and shadow, brute). Ours to change but the drawings.
Moves: Hammerhead, Rock, Greenstone, Entomb and Earthquake, with Gnaw, Ram,
Fluff Up and Assault. They take the fire line's places:

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Pebblub | Cotton Tunnel, 6–8 | Route 1, 12–15 | |
| Bouldrom | | Cotton Tunnel, 17–20 | Route 2, 26–30; Dragon's Cave, 30–32 |
| Tectorath | | | Dragon's Cave once Wyrmkeeper Ashe is beaten, 36–38 |

Scales 1/5, 1/3 and 1/2 (30 × 27, 35 × 38, 49 × 44): Tectorath's drawing has
5.3 picture pixels to its own, so 1/2 is as big as it goes, and Bouldrom stays
under it. Pebblub came through as drawn; Bouldrom's two glowing eyes and
Tectorath's three are drawn by hand in a kept pink.

## The eye line (2026-11-05)

The owner's eleventh line and the first of two stages: **Oglet** (water and
shadow, blob) evolves at 24 into **Oculoth** (water and shadow, serpent), and
no further. Ours to change but the drawings. Moves: Bubbles, Water Bullet,
Water Blast and Kraken, with Phantasmal Force, Mobbing and Strangulation. They
take the axolotl line's places:

| | Act 1 | Act 2 | Act 3 |
|---|---|---|---|
| Oglet | Brideswood, 5–7 | Brideswood, 14–17 | |
| Oculoth | | | Route 1, 22–26; Brideswood, 24–28 |

Scales 1/4 and 1/3 (32 × 34, 42 × 56), `"holes": []` for the bubbles. Their
many stalked eyes come through as cream and dark dots; nothing is drawn by
hand.
