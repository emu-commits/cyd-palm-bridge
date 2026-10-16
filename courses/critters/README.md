# Critters: the game file

`game.bin` is the data of **Critters**, the monster game in the Games folder
(`docs/GAME_PLAN.md`, format in `docs/GAME_FORMAT.md`). The device reads it
from `/sdcard/games/monster/game.bin`: copy it there.

**What's in it:** two connected areas of Tuxemon's Spyder maps (Route 1 and
Brideswood), 20 creatures in whole evolution lines, their moves, and the
player. Every tile, creature sprite and character sheet in it has a clean
licence (`docs/GAME_PLAN.md` §10 and §13 Q6): nothing under the XYG licence,
nothing non-commercial. Towns laid out from clean sheets come with G3.

**How it's built** (the output is the same to the byte every time):

    pip install -r tools/requirements-game.txt
    python3 tools/mkgame.py --fetch                       # Tuxemon at the pinned commit
    python3 tools/mkgame.py --strict -o courses/critters/game.bin

`tools/game_allow.tsv` says what may go in, `tools/game_region.json` which
maps, and `tools/game_ids.tsv` keeps the numbers a save refers to. The
Tuxemon checkout itself never enters this repository.

## Licences

- **Tuxemon's data** (creatures, moves, the maps' events and text) is under
  the **GPLv3**, like the firmware.
- **The art** is under **CC BY-SA 4.0**, each piece credited below. The file
  as a whole is shared under the same terms.
- **The player** is drawn for this project (`tools/gen_player_sheet.py`) and
  is public domain (**CC0 1.0**).

The device shows the same list under Menu ▸ Credits in the game.

## Credits

- Game data from Tuxemon (GPLv3), commit 9e6258ff726b.
- github.com/Tuxemon/Tuxemon

**Creatures**

- Agnidon: Leo (CC-BY-SA 4.0)
- Agnigon: Leo (CC-BY-SA 4.0)
- Agnite: Leo, Sanglorian, extyrannomon (CC-BY-SA 4.0)
- Anoleaf: Spalding004 (CC-BY-SA 4.0)
- Cardiling: Spalding004, Kyu (CC-BY-SA 4.0)
- Cardinale: Spalding004 (CC-BY-SA 4.0)
- Cardiwing: Spalding004 (CC-BY-SA 4.0)
- Dracune: Spalding004 (CC-BY-SA 4.0)
- Eyenemy: Cavalcadeur (CC-BY-SA 4.0)
- Eyesore: Cavalcadeur, josepharaoh99 (CC-BY-SA 4.0)
- Fluttaflap: Spalding004 (CC-BY-SA 4.0)
- Gectile: Spalding004 (CC-BY-SA 4.0)
- Noctalo: Catch Challenger (CC-BY-SA 4.0)
- Noctula: Catch Challenger (CC-BY-SA 4.0)
- Pigabyte: josepharaoh99 (CC-BY-SA 4.0)
- Selket: Catch Challenger (CC-BY-SA 4.0)
- Selmatek: Catch Challenger (CC-BY-SA 4.0)
- Vamporm: Spalding004 (CC-BY-SA 4.0)
- Velocitile: Spalding004 (CC-BY-SA 4.0)
- Zunna: Spalding004, Sanglorian (CC-BY-SA 4.0)

**Tiles**

- core_city_and_country: ArMM1998 (CC-BY-SA 4.0)
- core_indoor_stairs: rubberduck, ArMM1998, Redshrike, Mike Bramson (CC-BY-SA 4.0)
- core_outdoor: rubberduck, Buch, luke83, PastTheFuture, Midi, George_, ArMM1998, Redshrike, Isaiah658, Drummyfish, Sanglorian (CC-BY-SA 4.0)
- core_outdoor_nature: rubberduck, Buch, luke83, PastTheFuture, Midi, George_, ArMM1998, ZaPaper (CC-BY-SA 4.0)
- core_outdoor_water: rubberduck, Buch, luke83, PastTheFuture, Midi, George_, ArMM1998, ZaPaper (CC-BY-SA 4.0)

**Characters**

- player: CYD Palm (CC0 1.0)
