# Critters: the game file

`game.bin` is the data of **Critters**, the monster game in the Games folder
(`docs/GAME_PLAN.md`, format in `docs/GAME_FORMAT.md`). The device reads it
from `/sdcard/games/monster/game.bin`. The web flasher installs it when
"Also install Critters" is ticked: it goes into the board's `game` partition
with the firmware, and the board copies it to the card when it starts
(`firmware/main/monster_install.c`). Or copy it there by hand.

**What's in it:** the first region (G3): Larkhollow, where you start;
Route 1, the Brideswood and the Cotton Tunnel from Tuxemon's Spyder maps;
and Kestrel, with its Ring and Warden Ivy. The two towns are laid out for
this game (`tools/game_maps/`) from the clean tilesets. 27 creatures in
whole evolution lines and their moves; 17 townspeople and keepers, each
with words of our own; and six looks for the player. Every tile, creature
sprite and character sheet in it is on the allow-list with its licence
(`docs/GAME_PLAN.md` §10 and §13): nothing under the XYG licence, nothing
non-commercial. The townspeople are Tuxemon's own sheets; most of them come
from the Catch Challenger datapack, which Tuxemon credits as CC BY but whose
own repository states no licence. The owner chose to use them on Tuxemon's
word (2026-10-16); they're marked "per Tuxemon" below.

**How it's built** (the output is the same to the byte every time):

    pip install -r tools/requirements-game.txt
    python3 tools/mkgame.py --fetch                       # Tuxemon at the pinned commit
    python3 tools/mkgame.py --strict -o courses/critters/game.bin

`tools/game_allow.tsv` says what may go in, `tools/game_region.json` which
maps and who stands in them, `tools/game_maps/` the towns, and
`tools/game_ids.tsv` keeps the numbers a save refers to. The
Tuxemon checkout itself never enters this repository.

## Licences

- **Tuxemon's data** (creatures, moves, the maps' events and text) is under
  the **GPLv3**, like the firmware.
- **The art** is under **CC BY-SA 4.0**, each piece credited below. The file
  as a whole is shared under the same terms.
- **The player** (six looks) is drawn for this project
  (`tools/gen_player_sheet.py`) and is public domain (**CC0 1.0**).
- **The townspeople** are each under the licence listed beside them: CC BY-SA
  4.0, CC0, or CC BY (Catch Challenger's, per Tuxemon's credits).
- **The towns' layouts, the story and the dialogue** are ours, under the
  GPLv3 with the rest of this repository.

The device shows the same list under Menu ▸ Credits in the game.

## Credits

- Game data from Tuxemon (GPLv3), commit 9e6258ff726b.
- github.com/Tuxemon/Tuxemon

**Creatures**

- Agnidon: Leo (CC-BY-SA 4.0)
- Agnigon: Leo (CC-BY-SA 4.0)
- Agnite: Leo, Sanglorian, extyrannomon (CC-BY-SA 4.0)
- Anoleaf: Spalding004 (CC-BY-SA 4.0)
- Arthrobolt: josepharaoh99 (CC-BY-SA 4.0)
- Bolt: TacoBot (CC0 1.0)
- Cardiling: Spalding004, Kyu (CC-BY-SA 4.0)
- Cardinale: Spalding004 (CC-BY-SA 4.0)
- Cardiwing: Spalding004 (CC-BY-SA 4.0)
- Cateye: Cavalcadeur (CC-BY-SA 4.0)
- Djinnbo: Cavalcadeur, rsg167 (CC-BY-SA 4.0)
- Dracune: Spalding004 (CC-BY-SA 4.0)
- Dune Pincher: tamashihoshi (CC-BY-SA 4.0)
- Eyenemy: Cavalcadeur (CC-BY-SA 4.0)
- Eyesore: Cavalcadeur, josepharaoh99 (CC-BY-SA 4.0)
- Fluttaflap: Spalding004 (CC-BY-SA 4.0)
- Gectile: Spalding004 (CC-BY-SA 4.0)
- Noctalo: Catch Challenger (CC-BY-SA 4.0)
- Noctula: Catch Challenger (CC-BY-SA 4.0)
- Nut: TacoBot (CC0 1.0)
- Pigabyte: josepharaoh99 (CC-BY-SA 4.0)
- Propellercat: tamashihoshi (CC-BY-SA 4.0)
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

- catgirl: pboop, Sanglorian, tamashihoshi (CC-BY-SA 4.0)
- childactor: pboop (CC0 1.0)
- cooldude: Catch Challenger (Jordan Brule, Herman Brule) (CC BY 3.0, per Tuxemon)
- florist: Catch Challenger (Jordan Brule, Herman Brule) (CC BY 3.0, per Tuxemon)
- homemaker: Catch Challenger (Jordan Brule, Herman Brule) (CC BY 3.0, per Tuxemon)
- knightlord: Catch Challenger (Jordan Brule, Herman Brule) (CC BY 3.0, per Tuxemon)
- miner: Catch Challenger (Jordan Brule, Herman Brule), Sanglorian (CC BY 3.0, per Tuxemon)
- monk: Catch Challenger (Jordan Brule, Herman Brule), Sanglorian (CC BY 3.0, per Tuxemon)
- ninja: Pixel-boy (Sparklin Labs), Sanglorian (CC0 1.0)
- nurse: Catch Challenger (Jordan Brule, Herman Brule), Sanglorian (CC BY 3.0, per Tuxemon)
- picnicker: Catch Challenger (Jordan Brule, Herman Brule) (CC BY 3.0, per Tuxemon)
- player: CYD Palm (CC0 1.0)
- player_green: CYD Palm (CC0 1.0)
- player_olive: CYD Palm (CC0 1.0)
- player_orange: CYD Palm (CC0 1.0)
- player_rose: CYD Palm (CC0 1.0)
- player_violet: CYD Palm (CC0 1.0)
- postboy: Catch Challenger (Jordan Brule, Herman Brule), Sanglorian (CC BY 3.0, per Tuxemon)
- scientist: Catch Challenger (Jordan Brule, Herman Brule) (CC BY 3.0, per Tuxemon)
- shopassistant: Catch Challenger (Jordan Brule, Herman Brule) (CC BY 3.0, per Tuxemon)
- tennisplayer: pboop, Sanglorian (CC-BY-SA 4.0)
