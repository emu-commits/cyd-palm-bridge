#!/usr/bin/env python3
"""Write the monster game's test fixture source: a small Tuxemon-shaped tree.

    python3 tests/data/game/source.py DIR

The gates can't use Tuxemon itself (a 390 MB checkout, and its content
doesn't go into this repo: GAME_PLAN.md section 13 Q1), so this writes a
tree in Tuxemon's layout with content of our own, CC0: five invented
creatures, eleven moves, six types, two small maps, the tiles and sprites
drawn here. tools/mkgame.py builds it with --unpinned into game.bin beside
this file; tests/mkgame_test.py checks that the build is reproducible, and
make -C sim monster reads the result with the C reader.

It covers what the converter and the reader handle (with region.json and
village.json beside this file, which add a layout of our own, two looks for
the player, and people: a trainer with sight, a healer, a guard a flag sends
away, someone a flag brings): water (surfable 0),
walls (walkable 0), a ledge (hop, enter_from), a one-way cell, collision
rectangles and lines, overlays (the tree tops), warps both ways, encounter
zones with and without a chance, signs with a facing, events that are
dropped, a tileset not on the allow-list, a move with an effect the first
version doesn't play, a creature that can't be caught, a two-type move
against a two-type creature (the multiplier's clamp), a sprite with more
than 15 colours, and an area whose size isn't a multiple of a chunk.

Everything is drawn from numbers, so the tree is the same on every run (the
PNG bytes may differ between Pillow versions; mkgame reads only pixels).
"""
import base64
import struct
import sys
import zlib
from pathlib import Path

TYPES = ["fire", "metal", "normal", "venom", "water", "wood"]
# attacker -> {defender: multiplier}; anything not named is 1
MULT = {
    "fire": {"wood": 2, "metal": 2, "venom": 2, "water": 0.5, "fire": 0.5},
    "water": {"fire": 2, "wood": 0.5, "water": 0.5},
    "wood": {"water": 2, "fire": 0.5, "venom": 0.5, "metal": 0.5},
    "venom": {"wood": 2, "metal": 0.5},
    "metal": {"wood": 2, "fire": 0.5, "metal": 0.5},
    "normal": {"metal": 0.5},
}
SHAPES = {
    "blob": dict(hp=9, armour=7, dodge=4, melee=4, ranged=6, speed=4),
    "brute": dict(hp=7, armour=8, dodge=3, melee=8, ranged=3, speed=4),
    "sprite": dict(hp=5, armour=4, dodge=8, melee=5, ranged=7, speed=8),
}

# slug: (types, range, power, accuracy, potency, speed, recharge, effects, target, healing_power)
MOVES = {
    "scratch": (["normal"], "melee", 1.0, 1.0, 0, "normal", 0, [{"type": "damage"}], "enemy", 0),
    "ember": (["fire"], "ranged", 1.25, 0.9, 0.3, "normal", 1,
              [{"type": "damage"}, {"type": "give", "parameters": ["burn", "enemy_monster"]}], "enemy", 0),
    "splash_jet": (["water"], "reach", 1.5, 0.85, 0, "fast", 2, [{"type": "damage"}], "enemy", 0),
    "vine_whip": (["wood"], "touch", 1.2, 0.95, 0, "slow", 0, [{"type": "damage"}], "enemy", 0),
    "toxic_flare": (["fire", "venom"], "ranged", 1.0, 0.8, 0.5, "normal", 2,
                    [{"type": "damage"}, {"type": "give", "parameters": ["poison", "enemy_monster"]}], "enemy", 0),
    "rest": ([], "special", 0, 1.0, 0, "very_slow", 3, [{"type": "healing", "parameters": ["own_monster"]}],
             "own", 0.5),
    "soothe": (["water"], "special", 0, 1.0, 1.0, "normal", 4,
               [{"type": "give", "parameters": ["recover", "own_monster"]}], "own", 0),
    "bash": (["metal"], "melee", 2.0, 0.7, 0, "extremely_slow", 3, [{"type": "damage"}], "enemy", 0),
    "quick_jab": (["normal"], "touch", 0.75, 1.0, 0, "extremely_fast", 0, [{"type": "damage"}], "enemy", 0),
    "focus_beam": (["normal"], "reliable", 1.0, 1.0, 0, "normal", 1, [{"type": "damage"}], "enemy", 0),
    "frenzy": (["normal"], "melee", 1.5, 0.9, 0, "normal", 0, [{"type": "enraged"}], "enemy", 0),
}

# slug: (txmn_id, types, shape, stage, catch_rate, resist lo, hi, group, evolutions, moveset, colours)
CREATURES = {
    "emberkit": (901, ["fire"], "sprite", "basic", 100, 0.95, 1.25, "fast", [("blazecat", 6)],
                 [(1, "scratch"), (1, "ember"), (3, "quick_jab"), (5, "soothe"), (7, "frenzy"),
                  (8, "toxic_flare"), (9, "bash")], [(230, 90, 30), (250, 200, 60)]),
    "blazecat": (902, ["fire", "metal"], "brute", "stage1", 40, 1.0, 1.5, "fast", [],
                 [(1, "scratch"), (1, "ember"), (1, "bash"), (10, "rest"), (12, "toxic_flare")],
                 [(200, 60, 20), (120, 120, 130)]),
    "puddlop": (903, ["water"], "blob", "basic", 90, 0.9, 1.1, "slow", [("ghostling", 9)],
                [(1, "quick_jab"), (1, "splash_jet"), (4, "soothe"), (7, "rest")],
                [(40, 110, 220), (150, 210, 250)]),
    "thornet": (904, ["wood", "venom"], "sprite", "standalone", 70, 0.8, 1.2, "default", [],
                [(1, "vine_whip"), (1, "toxic_flare"), (6, "focus_beam")], None),   # a gradient: > 15 colours
    "rustmole": (905, ["metal"], "brute", "standalone", 0, 1.0, 1.0, "default", [],
                 [(1, "bash"), (1, "scratch"), (5, "focus_beam")], [(110, 80, 60), (170, 170, 160)]),
    "ghostling": (906, ["normal"], "blob", "stage1", 50, 1.0, 1.0, "default", [],
                  [(1, "scratch")], [(200, 200, 230), (90, 90, 120)]),   # in the tree, not on the allow-list
}

NAMES = {
    "fire": "Fire", "metal": "Metal", "normal": "Normal", "venom": "Venom", "water": "Water", "wood": "Wood",
    "poison": "Poison", "burn": "Burn", "recover": "Recover",
    "scratch": "Scratch", "ember": "Ember", "splash_jet": "Splash Jet", "vine_whip": "Vine Whip",
    "toxic_flare": "Toxic Flare", "rest": "Rest", "soothe": "Soothe", "bash": "Bash", "quick_jab": "Quick Jab",
    "focus_beam": "Focus Beam", "frenzy": "Frenzy",
    "emberkit": "Emberkit", "blazecat": "Blazecat", "puddlop": "Puddlop", "thornet": "Thornet",
    "rustmole": "Rustmole", "ghostling": "Ghostling",
    "fixture_sign_meadow": "Meadow. The burrow is east, past the trees.",
    "fixture_sign_burrow": "“Mind the moles” — the keeper",
}


def yaml_dump(v, ind=0):
    """Just enough YAML for these files (PyYAML isn't needed to write them)."""
    pad = "  " * ind
    out = []
    if isinstance(v, dict):
        for k, x in v.items():
            if isinstance(x, (dict, list)) and x:
                out.append(f"{pad}{k}:")
                out.append(yaml_dump(x, ind + 1))
            else:
                out.append(f"{pad}{k}: {scalar(x)}")
    else:
        for x in v:
            if isinstance(x, dict):
                lines = yaml_dump(x, ind + 1).split("\n")
                out.append(f"{pad}- {lines[0].lstrip()}")
                out += lines[1:]
            else:
                out.append(f"{pad}- {scalar(x)}")
    return "\n".join(out)


def scalar(x):
    if isinstance(x, bool):
        return "true" if x else "false"
    if isinstance(x, (dict, list)):
        return "{}" if isinstance(x, dict) else "[]"
    if isinstance(x, str):
        return "'" + x.replace("'", "''") + "'"
    return repr(x)


def write(p, s):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(s, encoding="utf-8")


# ---------------------------------------------------------------- pictures

def tileset_image(kind):
    from PIL import Image, ImageDraw
    if kind == "fixture_tiles":
        im = Image.new("RGBA", (64, 48), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)

        def at(i):
            return (i % 4) * 16, (i // 4) * 16
        # 0 grass, 1 tall grass, 2 water, 3 path, 4 trunk, 5 tree top, 6 ledge,
        # 7 fence, 8 cave floor, 9 rock wall, 10 sign, 11 cave mouth
        for i, base in ((0, (90, 170, 70)), (1, (60, 140, 40)), (2, (50, 100, 210)), (3, (200, 180, 120)),
                        (4, (90, 170, 70)), (6, (90, 170, 70)), (7, (90, 170, 70)), (8, (110, 100, 90)),
                        (9, (70, 60, 55)), (10, (200, 180, 120)), (11, (30, 25, 25))):
            x, y = at(i)
            d.rectangle((x, y, x + 15, y + 15), fill=base + (255,))
        x, y = at(1)
        for k in range(0, 16, 4):
            d.line((x + k, y + 15, x + k + 2, y + 4), fill=(170, 210, 60, 255))
        x, y = at(2)
        d.line((x + 2, y + 5, x + 7, y + 5), fill=(200, 230, 255, 255))
        d.line((x + 8, y + 11, x + 13, y + 11), fill=(200, 230, 255, 255))
        x, y = at(4)
        d.rectangle((x + 5, y, x + 10, y + 15), fill=(110, 70, 40, 255))
        x, y = at(5)
        d.ellipse((x, y + 2, x + 15, y + 15), fill=(30, 110, 50, 255))
        x, y = at(6)
        d.rectangle((x, y + 10, x + 15, y + 15), fill=(120, 90, 50, 255))
        x, y = at(7)
        d.rectangle((x, y + 6, x + 15, y + 8), fill=(160, 120, 80, 255))
        x, y = at(9)
        d.line((x, y + 8, x + 15, y + 8), fill=(40, 35, 30, 255))
        x, y = at(10)
        d.rectangle((x + 3, y + 3, x + 12, y + 10), fill=(150, 100, 60, 255))
        d.rectangle((x + 7, y + 10, x + 8, y + 15), fill=(110, 70, 40, 255))
        return im
    im = Image.new("RGBA", (32, 16), (0, 0, 0, 0))   # fixture_decor: flower, mushroom
    d = ImageDraw.Draw(im)
    d.ellipse((4, 4, 11, 11), fill=(230, 60, 90, 255))
    d.ellipse((20, 3, 27, 9), fill=(220, 40, 40, 255))
    d.rectangle((23, 9, 24, 14), fill=(240, 230, 210, 255))
    return im


def sprite_sheet(slug, colours):
    """128 x 88, the front at (0,0) and the back at (64,0), as Tuxemon's."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (128, 88), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    n = sum(map(ord, slug))
    for f in range(2):
        ox = 64 * f
        if colours is None:
            # a vertical gradient: 40 colours, so the build has to reduce them
            for y in range(8, 58):
                d.line((ox + 14, y, ox + 50, y), fill=(40 + y * 3, 160 - y, 60 + (y * 7) % 50, 255))
        else:
            body, mark = colours
            d.ellipse((ox + 10 + n % 5, 12, ox + 54, 58), fill=body + (255,))
            d.ellipse((ox + 22, 30, ox + 42, 50), fill=mark + (255,))
        if f == 0:
            d.rectangle((ox + 24, 22, ox + 27, 26), fill=(20, 20, 20, 255))
            d.rectangle((ox + 36, 22, ox + 39, 26), fill=(20, 20, 20, 255))
    return im


def character_sheet(shirt=(200, 40, 40)):
    """48 x 128: twelve 16 x 32 frames, three per direction."""
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (48, 128), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for f in range(12):
        x, y = f % 3 * 16, f // 3 * 32
        d.ellipse((x + 4, y + 8, x + 11, y + 15), fill=(240, 200, 160, 255))      # head
        d.rectangle((x + 4, y + 16, x + 11, y + 25), fill=shirt + (255,))          # shirt
        step = (f % 3) - 1
        d.rectangle((x + 4 + step, y + 26, x + 6 + step, y + 31), fill=(40, 40, 120, 255))
        d.rectangle((x + 9 - step, y + 26, x + 11 - step, y + 31), fill=(40, 40, 120, 255))
    return im


# ---------------------------------------------------------------- maps

def layer_xml(name, w, h, cells):
    raw = struct.pack(f"<{w * h}I", *cells)
    data = base64.b64encode(zlib.compress(raw, 9)).decode()
    return (f' <layer name="{name}" width="{w}" height="{h}">\n'
            f'  <data encoding="base64" compression="zlib">{data}</data>\n </layer>\n')


def props_xml(p, ind="   "):
    if not p:
        return ""
    s = f"{ind}<properties>\n"
    for k, v in p.items():
        s += f'{ind} <property name="{k}" value="{v}"/>\n'
    return s + f"{ind}</properties>\n"


TILE_PROPS = {2: {"surfable": "0"}, 6: {"hop": "true", "enter_from": "up"}, 9: {"walkable": "0"}}


def tilesets_xml():
    s = ' <tileset firstgid="1" name="fixture_tiles" tilewidth="16" tileheight="16" tilecount="12" columns="4">\n'
    s += '  <image source="../gfx/tilesets/fixture_tiles.png" width="64" height="48"/>\n'
    for t, p in TILE_PROPS.items():
        s += f'  <tile id="{t}">\n' + props_xml(p) + "  </tile>\n"
    s += " </tileset>\n"
    s += ' <tileset firstgid="13" name="fixture_decor" tilewidth="16" tileheight="16" tilecount="2" columns="2">\n'
    s += '  <image source="../gfx/tilesets/fixture_decor.png" width="32" height="16"/>\n </tileset>\n'
    return s


def obj(i, x, y, w, h, kind, props=None, poly=None):
    s = f'  <object id="{i}" type="{kind}" x="{x * 16}" y="{y * 16}"'
    if w or h:
        s += f' width="{w * 16}" height="{h * 16}"'
    s += ">\n" + props_xml(props or {})
    if poly:
        s += '   <polyline points="' + " ".join(f"{a * 16},{b * 16}" for a, b in poly) + '"/>\n'
    return s + "  </object>\n"


def map_xml(w, h, layers, objects):
    s = ('<?xml version="1.0" encoding="UTF-8"?>\n'
         f'<map version="1.10" orientation="orthogonal" renderorder="right-down" width="{w}" height="{h}" '
         'tilewidth="16" tileheight="16" infinite="0">\n')
    s += tilesets_xml()
    for name, cells in layers:
        s += layer_xml(name, w, h, cells)
    s += ' <objectgroup name="events">\n' + "".join(objects) + " </objectgroup>\n</map>\n"
    return s


def meadow():
    w, h = 24, 20
    ground, detail, decor, above = [1] * (w * h), [0] * (w * h), [0] * (w * h), [0] * (w * h)

    def put(layer, x, y, gid):
        layer[y * w + x] = gid
    for y in range(h):
        for x in (3, 4):
            put(ground, x, y, 4)                         # path
    for y in range(3, 7):
        for x in range(14, 19):
            put(ground, x, y, 3)                         # water
    for y in range(10, 16):
        for x in range(6, 12):
            put(detail, x, y, 2)                         # tall grass
    for x in range(6, 13):
        put(detail, x, 8, 7)                             # the ledge
    for x in (20, 21):
        put(detail, x, 12, 5)                            # trunks
        put(above, x, 11, 6)                             # their tops, over the player
    put(detail, 5, 2, 11)                                # the sign
    put(detail, 23, 9, 12)                               # the burrow's mouth
    put(decor, 8, 2, 13)                                 # a flower from the unlisted tileset
    for x in range(0, 10):
        put(detail, x, 17, 8)                            # the fence's picture
    objects = [
        obj(1, 20, 12, 2, 1, "collision"),
        obj(2, 0, 0, 0, 0, "collision", poly=[(0, 17), (10, 17)]),       # the fence: a line along y = 17
        obj(3, 0, 0, 0, 0, "collision", poly=[(13, 10), (13, 14)]),      # a wall along x = 13
        obj(4, 12, 2, 1, 1, "collision", {"enter_from": "down"}),        # a one-way gate
        obj(5, 6, 10, 6, 6, "event", {"act1": "random_encounter fixture_meadow,11"}),
        obj(6, 23, 9, 1, 1, "event", {"act1": "transition_teleport player,fixture_burrow.tmx,1,8,0.3",
                                      "cond1": "is char_facing player,right"}),
        obj(7, 5, 2, 1, 1, "event", {"act1": "translated_dialog fixture_sign_meadow",
                                     "cond1": "is char_facing player,up"}),
        obj(8, 0, 19, 24, 1, "event", {"act1": "transition_teleport player,fixture_elsewhere.tmx,3,0,0.3"}),
        obj(9, 1, 1, 1, 1, "event", {"act1": "set_variable fixture_seen:yes"}),
    ]
    return map_xml(w, h, [("ground", ground), ("detail", detail), ("decor", decor), ("above", above)], objects)


def burrow():
    w, h = 18, 17                                        # not a multiple of a chunk either way
    ground, detail, decor = [9] * (w * h), [0] * (w * h), [0] * (w * h)
    for y in range(h):
        for x in range(w):
            if x in (0, w - 1) or y in (0, h - 1):
                ground[y * w + x] = 10                   # rock wall
    ground[8 * w + 0] = 12                               # the way out
    for x, y in ((4, 4), (13, 12), (9, 3)):
        decor[y * w + x] = 14                            # mushrooms, unlisted
    objects = [
        obj(1, 0, 8, 1, 1, "event", {"act1": "transition_teleport player,fixture_meadow.tmx,22,9,0.3",
                                     "cond1": "is char_facing player,left"}),
        obj(2, 5, 5, 8, 7, "event", {"act1": "random_encounter fixture_burrow"}),
        obj(3, 2, 2, 1, 1, "event", {"act1": "translated_dialog fixture_sign_burrow"}),
    ]
    return map_xml(w, h, [("ground", ground), ("detail", detail), ("decor", decor)], objects)


# ---------------------------------------------------------------- the tree

def main(out):
    out = Path(out)
    mods = out / "mods"
    tux = mods / "tuxemon"
    db = tux / "db"
    write(mods / "config_combat.yaml", yaml_dump({"multiplier_range": [0.25, 4.0], "speed_factor": 0.25}) + "\n")
    write(mods / "config_capture.yaml", yaml_dump({
        "total_shakes": 4, "shake_constant": 524325, "shake_denominator": 8, "shake_divisor": 65536,
        "shake_hp_multiplier": 3, "shake_current_hp_multiplier": 2, "shake_hp_divisor": 3}) + "\n")
    write(mods / "config_monster.yaml", yaml_dump({
        "level_range": [0, 100], "max_moves": 4, "iv_range": [0, 15], "coeff_stats": 7,
        "experience_groups": {
            "default": {"multiplier": 1.0, "experience_coefficient": 3},
            "fast": {"multiplier": 0.8, "experience_coefficient": 3},
            "slow": {"multiplier": 1.25, "experience_coefficient": 3}}}) + "\n")
    write(mods / "range_map.yaml", yaml_dump({r: [{"weight": 1.0}] for r in
                                              ("melee", "touch", "ranged", "reach", "reliable", "special")}) + "\n")
    for t in TYPES:
        write(db / "element" / f"{t}.yaml", yaml_dump({
            "slug": t, "types": [{"against": u, "multiplier": MULT.get(t, {}).get(u, 1)} for u in TYPES]}) + "\n")
    write(db / "shape" / "shapes.yaml",
          yaml_dump([{"attributes": a, "slug": s} for s, a in SHAPES.items()]) + "\n")
    statuses = {
        "poison": {"modifiers": [{"attribute": "type", "values": ["venom"], "multiplier": 0.0}],
                   "behaviors": {"persists_after_combat": True}, "category": "negative",
                   "effects": [{"type": "poisoned", "parameters": ["8", "weakest"]}], "slug": "poison"},
        "burn": {"modifiers": [{"attribute": "type", "values": ["fire"], "multiplier": 0.0}],
                 "behaviors": {"persists_after_combat": True}, "category": "negative",
                 "effects": [{"type": "burnt", "parameters": ["8"]}], "slug": "burn"},
        "recover": {"modifiers": [], "behaviors": {}, "category": "positive",
                    "effects": [{"type": "recover", "parameters": ["16"]}], "slug": "recover"},
    }
    for s, d in statuses.items():
        write(db / "status" / f"{s}.yaml", yaml_dump(d) + "\n")
    for i, (s, (ty, rng, power, acc, pot, speed, rech, effs, tgt, heal)) in enumerate(sorted(MOVES.items()), 1):
        d = {"tech_id": i, "accuracy": acc, "effects": effs, "speed": speed, "potency": pot, "power": power,
             "range": rng, "recharge": rech, "slug": s, "sort": "damage",
             "target": {"enemy_monster": tgt == "enemy", "own_monster": tgt == "own"}, "types": ty}
        if heal:
            d["healing_power"] = heal
        write(db / "technique" / f"{s}.yaml", yaml_dump(d) + "\n")
    for s, (txmn, ty, shape, stage, cr, lo, hi, grp, evo, learn, colours) in CREATURES.items():
        d = {"slug": s, "moveset": [{"level_learned": lv, "technique": t} for lv, t in learn],
             "evolutions": [{"at_level": lv, "monster_slug": to} for to, lv in evo],
             "shape": shape, "stage": stage, "types": ty, "txmn_id": txmn, "catch_rate": float(cr),
             "lower_catch_resistance": lo, "upper_catch_resistance": hi}
        if grp != "default":
            d["experience_group"] = grp
        write(db / "monster" / f"{s}.yaml", yaml_dump(d) + "\n")
        p = tux / "gfx" / "sprites" / "battle" / f"{s}-sheet.png"
        p.parent.mkdir(parents=True, exist_ok=True)
        sprite_sheet(s, colours).save(p)
    for kind in ("fixture_tiles", "fixture_decor"):
        p = tux / "gfx" / "tilesets" / f"{kind}.png"
        p.parent.mkdir(parents=True, exist_ok=True)
        tileset_image(kind).save(p)
    p = tux / "sprites" / "fixture_walker.png"
    p.parent.mkdir(parents=True, exist_ok=True)
    character_sheet().save(p)
    character_sheet((40, 90, 200)).save(tux / "sprites" / "fixture_rover.png")     # the player's second look
    character_sheet((40, 150, 60)).save(tux / "sprites" / "fixture_keeper.png")    # the people
    write(tux / "maps" / "fixture_meadow.tmx", meadow())
    write(tux / "maps" / "fixture_burrow.tmx", burrow())
    po = '# the fixture\'s names\nmsgid ""\nmsgstr ""\n"Content-Type: text/plain; charset=UTF-8\\n"\n\n'
    for k, v in sorted(NAMES.items()):
        po += f'msgid "{k}"\nmsgstr "{v}"\n\n'
    write(tux / "l18n" / "en_US" / "LC_MESSAGES" / "base.po", po)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
