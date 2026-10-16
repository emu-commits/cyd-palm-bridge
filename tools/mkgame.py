#!/usr/bin/env python3
"""Build the monster game's data file (game.bin) from Tuxemon's open content.

    python3 tools/mkgame.py --fetch                      # clone Tuxemon at the pinned commit
    python3 tools/mkgame.py -o game.bin                  # build
    python3 tools/mkgame.py -o game4.bin --bpp 4         # map tiles at 4 bits per pixel
    python3 tools/mkgame.py --check game.bin             # validate a built file, print sizes
    python3 tools/mkgame.py --dump game.bin              # canonical text dump
    python3 tools/mkgame.py --preview DIR game.bin ...   # draw what the device would draw

The format is docs/GAME_FORMAT.md. The plan is docs/GAME_PLAN.md: the engine
is ours, the content is Tuxemon's (github.com/Tuxemon/Tuxemon, GPLv3 code and
data, mostly CC-BY-SA art), read from a checkout at one pinned commit and
never vendored into this repo.

ONLY WHAT tools/game_allow.tsv LISTS IS BUILT. A map cell drawn from a tileset
that isn't listed loses that tile; a creature that isn't listed can't be in an
encounter or an evolution; the build says how much was left out.

The region (which maps, where the player starts, which creatures live where)
is tools/game_region.json.

Builds are reproducible to the byte: nothing depends on time, the machine or
dict order, and Pillow and PyYAML are pinned (tools/requirements-game.txt).
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import zlib
import base64
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mkcourse import fold, latin_fallback  # noqa: E402  (one set of text rules)

FORMAT_MAJOR, FORMAT_MINOR = 1, 0
MAGIC = b"CYDG"

TUXEMON_URL = "https://github.com/Tuxemon/Tuxemon.git"
TUXEMON_COMMIT = "9e6258ff726b786040a267e8bdbbf037b560285e"   # development, 2026-09-28
CACHE = Path.home() / ".cache" / "cyd-palm" / "tuxemon"

TOOLS = Path(__file__).resolve().parent
ALLOW = TOOLS / "game_allow.tsv"
REGION = TOOLS / "game_region.json"
IDS = TOOLS / "game_ids.tsv"

TILE = 16
CHUNK = 16                    # cells per chunk side: a screen spans at most 2 x 2 (GAME_FORMAT.md 3.11)
SPRITE_W = SPRITE_H = 64      # battle sprites
CHAR_W, CHAR_H, CHAR_FRAMES = 16, 32, 12
BELOW_LAYERS = 3              # Tuxemon draws characters after tile layer index 2
VIEW_W, VIEW_H = 15, 11       # the game view in cells (240 x 176)
MAX_AREA_CELLS = 255          # width and height each

SECTIONS = [b"META", b"TYPE", b"SHAP", b"STAT", b"MOVE", b"MONS", b"LRNS",
            b"ENCT", b"AREA", b"ADAT", b"SPRT", b"TEXT", b"CRED"]
CRC_ON_OPEN = {b"META", b"TYPE", b"SHAP", b"STAT", b"MOVE", b"MONS", b"LRNS",
               b"ENCT", b"AREA", b"TEXT"}

# META keys (GAME_FORMAT.md 3.2)
K_TITLE, K_SOURCE, K_BUILD = 0x01, 0x02, 0x03
K_START = 0x10
K_RULES, K_CAPTURE, K_GROWTH = 0x20, 0x21, 0x22

RANGES = ["melee", "touch", "ranged", "reach", "reliable", "special"]
SPEEDS = ["extremely_slow", "very_slow", "slow", "normal", "fast", "very_fast", "extremely_fast"]
STAGES = ["basic", "stage1", "stage2", "standalone"]
EXP_GROUPS = ["default", "fast", "slow"]
DIRS = {"up": 0, "right": 1, "down": 2, "left": 3}          # N E S W
DIR_DELTA = [(0, -1), (1, 0), (0, 1), (-1, 0)]

# effects (GAME_FORMAT.md 3.5)
E_DAMAGE, E_HEALING, E_GIVE = 1, 2, 3
T_ENEMY, T_OWN = 0, 1
# statuses the first version plays (GAME_PLAN.md section 6)
STATUS_KINDS = {"poison": 1, "burn": 2, "recover": 3}
STATUS_EFFECT = {"poisoned": 1, "burnt": 2, "recover": 3}


class GameError(Exception):
    pass


def fail(msg):
    raise GameError(msg)


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def pad4(b):
    return b + b"\0" * (-len(b) % 4)


def rgb565(c):
    r, g, b = c[:3]
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def rgb888(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def text_ok(s, what):
    s = latin_fallback(fold(s)).strip()
    bad = sorted({ch for ch in s if not 0x20 <= ord(ch) <= 0xFF})
    if bad:
        fail(f"{what}: characters the Palm font can't draw: {''.join(bad)!r}")
    return s


# ---------------------------------------------------------------- the source

def need_yaml():
    try:
        import yaml
    except ImportError:
        fail("PyYAML is needed: pip install -r tools/requirements-game.txt")
    return yaml


def fetch(dest):
    dest = Path(dest)
    if not (dest / ".git").exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "clone", "--filter=blob:none", "--no-checkout",
                        TUXEMON_URL, str(dest)], check=True)
    subprocess.run(["git", "-C", str(dest), "fetch", "-q", "origin", TUXEMON_COMMIT], check=False)
    subprocess.run(["git", "-C", str(dest), "-c", "advice.detachedHead=false",
                    "checkout", "-q", TUXEMON_COMMIT], check=True)
    return dest


def check_source(root):
    """The checkout must be the pinned commit, with nothing changed in what we read."""
    root = Path(root)
    if not (root / "mods" / "tuxemon" / "db").is_dir():
        fail(f"{root} isn't a Tuxemon checkout (run --fetch, or pass --tuxemon)")
    try:
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], check=True,
                              capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(root), "status", "--porcelain", "--", "mods"],
                               check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        fail(f"{root}: can't ask git which commit this is")
    if head != TUXEMON_COMMIT:
        fail(f"{root} is at {head[:12]}, not the pinned {TUXEMON_COMMIT[:12]}")
    if dirty:
        fail(f"{root} has local changes under mods/:\n{dirty}")
    return root


class Po:
    """msgid -> msgstr from Tuxemon's base.po (plain entries only)."""

    def __init__(self, path):
        self.m = {}
        cur, key, val = None, None, None
        held = ""
        for line in open(path, encoding="utf-8"):
            line = held + line.strip()
            held = ""
            # a string broken by a raw line break (base.po has one): join it
            if len(re.findall(r'(?<!\\)"', line.replace("\\\\", ""))) % 2:
                held = line + " "
                continue
            if line.startswith("msgid "):
                key, val, cur = self._q(line[6:]), None, "id"
            elif line.startswith("msgstr "):
                val, cur = self._q(line[7:]), "str"
            elif line.startswith('"') and cur:
                if cur == "id":
                    key += self._q(line)
                else:
                    val += self._q(line)
            else:
                if key and val is not None:
                    self.m[key] = val
                cur = None
        if key and val is not None:
            self.m[key] = val

    @staticmethod
    def _q(s):
        s = s.strip()
        if len(s) < 2 or s[0] != '"' or s[-1] != '"':
            fail(f"base.po: not a quoted string: {s[:40]}")
        esc = {"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}
        return re.sub(r"\\(.)", lambda m: esc.get(m.group(1), m.group(1)), s[1:-1])

    def get(self, k, default=None):
        v = self.m.get(k)
        return v if v else default


def load_allow(path=ALLOW):
    allow = {"tileset": {}, "creature": {}, "character": {}}
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = line.rstrip("\n")
        if not line.strip() or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) != 6 or f[0] not in allow or f[5] not in ("credited", "unverified", "confirmed"):
            fail(f"{path}:{n}: want kind, slug, licence, authors, source, status")
        allow[f[0]][f[1]] = {"licence": f[2], "authors": f[3], "source": f[4], "status": f[5]}
    return allow


def load_ids(path=IDS):
    ids = {}
    if Path(path).exists():
        for n, line in enumerate(open(path, encoding="utf-8"), 1):
            if not line.strip() or line.startswith("#"):
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) != 3 or not f[2].isdigit():
                fail(f"{path}:{n}: want kind, slug, number")
            ids[(f[0], f[1])] = int(f[2])
    return ids


def assign_id(ids, kind, slug):
    if (kind, slug) not in ids:
        ids[(kind, slug)] = max([v for (k, _), v in ids.items() if k == kind], default=0) + 1
    return ids[(kind, slug)]


def save_ids(ids, path=IDS):
    lines = ["# kind<TAB>slug<TAB>number: stable numbers for what a save refers to.",
             "# Maintained by tools/mkgame.py; commit it. Never renumber or reuse a number."]
    for (k, s), v in sorted(ids.items(), key=lambda kv: (kv[0][0], kv[1])):
        lines.append(f"{k}\t{s}\t{v}")
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


# ---------------------------------------------------------------- the rules

class Rules:
    def __init__(self, root, allow, region, report):
        yaml = need_yaml()
        self.root = root
        db = root / "mods" / "tuxemon" / "db"
        self.po = Po(root / "mods" / "tuxemon" / "l18n" / "en_US" / "LC_MESSAGES" / "base.po")
        self.report = report

        def y(p):
            return yaml.safe_load(open(p, encoding="utf-8"))

        cfg = root / "mods"
        self.combat = y(cfg / "config_combat.yaml")
        self.capture = y(cfg / "config_capture.yaml")
        self.monster_cfg = y(cfg / "config_monster.yaml")
        self.range_map = y(cfg / "range_map.yaml")

        self.types = sorted(p.stem for p in (db / "element").glob("*.yaml"))
        self.type_table = []
        for t in self.types:
            d = y(db / "element" / f"{t}.yaml")
            m = {e["against"]: e["multiplier"] for e in d["types"]}
            self.type_table.append([self._quarter(m.get(u, 1), f"type {t} vs {u}") for u in self.types])

        self.shapes = sorted(y(db / "shape" / "shapes.yaml"), key=lambda s: s["slug"])
        self.shape_ix = {s["slug"]: i for i, s in enumerate(self.shapes)}

        self.statuses = {}
        for slug in STATUS_KINDS:
            self.statuses[slug] = y(db / "status" / f"{slug}.yaml")

        self.mons = {}
        for slug in sorted(allow["creature"]):
            p = db / "monster" / f"{slug}.yaml"
            if not p.exists():
                fail(f"game_allow.tsv: creature {slug} isn't in Tuxemon")
            self.mons[slug] = y(p)
        self.techs = {}
        unsupported = {}
        for slug in sorted(self.mons):
            for mv in self.mons[slug]["moveset"]:
                t = mv["technique"]
                if t in self.techs or t in unsupported:
                    continue
                d = y(db / "technique" / f"{t}.yaml")
                why = self._unsupported(d)
                if why:
                    unsupported[t] = why
                else:
                    self.techs[t] = d
        self.unsupported = unsupported
        report.append(f"moves: {len(self.techs)} kept, {len(unsupported)} left out "
                      f"(effects the first version doesn't play)")
        for t, why in sorted(unsupported.items()):
            report.append(f"  left out {t}: {why}")

        self.encounters = {}
        for slug, rows in sorted(region.get("encounters", {}).items()):
            for r in rows:
                if r["creature"] not in self.mons:
                    fail(f"game_region.json: encounter {slug} uses {r['creature']}, not in the allow-list")
                lo, hi = r["levels"]
                if not 1 <= lo <= hi <= 100:
                    fail(f"game_region.json: encounter {slug}: levels {lo}-{hi}")
            self.encounters[slug] = rows

    @staticmethod
    def _quarter(x, what):
        q = x * 4
        if q != int(q) or not 0 <= q <= 255:
            fail(f"{what}: multiplier {x} isn't a multiple of 0.25")
        return int(q)

    def _unsupported(self, d):
        if d.get("range") not in RANGES:
            return f"range {d.get('range')}"
        for e in d.get("effects", []):
            t = e["type"]
            p = e.get("parameters") or []
            if t == "damage" and not p:
                continue
            if t == "healing" and p in ([], ["own_monster"], ["enemy_monster"]):
                continue
            if t == "give" and len(p) == 2 and p[0] in STATUS_KINDS and p[1] in ("own_monster", "enemy_monster"):
                continue
            return f"effect {t} {' '.join(map(str, p))}".strip()
        if not d.get("effects"):
            return "no effects"
        return None

    def name(self, slug):
        return self.po.get(slug, slug.replace("_", " ").title())


# ---------------------------------------------------------------- the maps

def parse_props(el):
    out = {}
    if el is None:
        return out
    ps = el.find("properties")
    if ps is not None:
        for p in ps.findall("property"):
            out[p.get("name")] = p.get("value") if p.get("value") is not None else (p.text or "")
    return out


def dir_list(v):
    return [DIRS[d.strip()] for d in (v or "").split(",") if d.strip() in DIRS]


class Sheet:
    def __init__(self, el, base, cache):
        from PIL import Image
        src = el.get("source")
        if src:
            p = os.path.normpath(os.path.join(base, src))
            t = ET.parse(p).getroot()
            base = os.path.dirname(p)
            self.name = Path(src).stem
        else:
            t = el
            self.name = el.get("name")
        if int(t.get("tilewidth")) != TILE or int(t.get("tileheight")) != TILE:
            fail(f"tileset {self.name}: tiles aren't {TILE}x{TILE}")
        img = t.find("image")
        ip = os.path.normpath(os.path.join(base, img.get("source")))
        if ip not in cache:
            cache[ip] = Image.open(ip).convert("RGBA")
        self.image = cache[ip]
        self.columns = int(t.get("columns"))
        self.count = int(t.get("tilecount"))
        self.props = {int(x.get("id")): parse_props(x) for x in t.findall("tile")}

    def tile(self, k):
        x, y = k % self.columns * TILE, k // self.columns * TILE
        return self.image.crop((x, y, x + TILE, y + TILE))


class Area:
    """One Tuxemon map, read with only the allow-listed tilesets."""

    def __init__(self, root, slug, allowed, cache, report):
        self.slug = slug
        mapdir = root / "mods" / "tuxemon" / "maps"
        m = ET.parse(mapdir / f"{slug}.tmx").getroot()
        if m.get("orientation") != "orthogonal" or m.get("infinite") == "1":
            fail(f"{slug}: only orthogonal, finite maps")
        self.w, self.h = int(m.get("width")), int(m.get("height"))
        if not (1 <= self.w <= MAX_AREA_CELLS and 1 <= self.h <= MAX_AREA_CELLS):
            fail(f"{slug}: {self.w}x{self.h} cells; the limit is {MAX_AREA_CELLS} a side")
        self.props = parse_props(m)
        sheets = sorted(((int(t.get("firstgid")), Sheet(t, str(mapdir), cache))
                         for t in m.findall("tileset")), key=lambda x: x[0])
        self.layers = []
        lost = {}
        for L in m.findall("layer"):
            if L.get("visible") == "0":
                continue
            d = L.find("data")
            if d.get("encoding") != "base64":
                fail(f"{slug}: layer {L.get('name')}: only base64 data")
            raw = base64.b64decode(d.text.strip())
            if d.get("compression") == "zlib":
                raw = zlib.decompress(raw)
            elif d.get("compression"):
                fail(f"{slug}: {d.get('compression')} compression")
            gids = struct.unpack(f"<{self.w * self.h}I", raw)
            cells = []
            for g in gids:
                flip, g = g >> 28, g & 0x0FFFFFFF
                if not g:
                    cells.append(None)
                    continue
                if flip:
                    fail(f"{slug}: flipped tiles aren't supported")
                fg, sh = [s for s in sheets if s[0] <= g][-1]
                if sh.name not in allowed:
                    lost[sh.name] = lost.get(sh.name, 0) + 1
                    cells.append(None)
                    continue
                cells.append((sh, g - fg))
            self.layers.append((L.get("name"), cells))
        self.lost = lost
        n_tiles = sum(1 for _, c in self.layers for x in c if x)
        if lost:
            report.append(f"{slug}: {sum(lost.values())} of {n_tiles + sum(lost.values())} layer tiles "
                          f"left out, from {', '.join(f'{k} ({v})' for k, v in sorted(lost.items()))}")
        self.objects = []
        for g in m.findall("objectgroup"):
            for o in g.findall("object"):
                self.objects.append(o)

    def cell_rect(self, o):
        x, y = float(o.get("x")), float(o.get("y"))
        w, h = float(o.get("width", 0)), float(o.get("height", 0))
        x0, y0 = int(x // TILE), int(y // TILE)
        x1 = max(x0 + 1, int(-(-(x + w) // TILE)))
        y1 = max(y0 + 1, int(-(-(y + h) // TILE)))
        return (max(0, x0), max(0, y0), min(self.w, x1), min(self.h, y1))

    def movement(self):
        """One byte per cell (GAME_FORMAT.md 3.9): bits 0-3 can't enter across
        the N E S W side; bits 4-7 can't leave across it."""
        mv = [0] * (self.w * self.h)
        hop = set()
        # a surface at 0 (water to someone who can't swim) is solid; the tile
        # and object rules below override it, as Tuxemon's collision map does
        for _, cells in self.layers:
            for i, c in enumerate(cells):
                if c:
                    p = c[0].props.get(c[1], {})
                    if any(k in p and float(p[k] or 0) == 0 for k in ("surfable", "walkable", "climbable")):
                        mv[i] = 0x0F
        for _, cells in self.layers:
            for i, c in enumerate(cells):
                if not c:
                    continue
                p = c[0].props.get(c[1], {})
                if p.get("hop") == "true":
                    hop.add(i)
                if not any(k in p for k in ("enter_from", "exit_from", "endure")):
                    continue
                enter = dir_list(p.get("enter_from"))
                leave = dir_list(p.get("exit_from")) + dir_list(p.get("endure"))
                b = sum(1 << d for d in range(4) if d not in enter)
                if p.get("exit_from") or p.get("endure"):
                    b |= sum(16 << d for d in range(4) if d not in leave)
                mv[i] = b
        for o in self.objects:
            t = (o.get("type") or "").lower()
            if not t.startswith("collision"):
                continue
            poly = o.find("polyline")
            if poly is not None:
                ox, oy = float(o.get("x")), float(o.get("y"))
                pts = [(ox + float(a), oy + float(b)) for a, b in
                       (p.split(",") for p in poly.get("points").split())]
                for (ax, ay), (bx, by) in zip(pts, pts[1:]):
                    self._wall(mv, ax, ay, bx, by)
                continue
            p = parse_props(o)
            x0, y0, x1, y1 = self.cell_rect(o)
            if any(k in p for k in ("enter_from", "exit_from", "endure")):
                enter = dir_list(p.get("enter_from"))
                leave = dir_list(p.get("exit_from")) + dir_list(p.get("endure"))
                b = sum(1 << d for d in range(4) if d not in enter)
                if p.get("exit_from") or p.get("endure"):
                    b |= sum(16 << d for d in range(4) if d not in leave)
            else:
                b = 0x0F
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    mv[yy * self.w + xx] = b
        return mv, hop

    def _wall(self, mv, ax, ay, bx, by):
        """A collision line along cell edges blocks crossing it either way."""
        if ay == by:
            row = round(ay / TILE)
            for x in range(int(min(ax, bx) // TILE), int(-(-max(ax, bx) // TILE))):
                if 0 <= x < self.w:
                    if 0 <= row - 1 < self.h:
                        mv[(row - 1) * self.w + x] |= (1 << 2) | (16 << 2)   # S side
                    if 0 <= row < self.h:
                        mv[row * self.w + x] |= 1 | 16                     # N side
        elif ax == bx:
            col = round(ax / TILE)
            for y in range(int(min(ay, by) // TILE), int(-(-max(ay, by) // TILE))):
                if 0 <= y < self.h:
                    if 0 <= col - 1 < self.w:
                        mv[y * self.w + col - 1] |= (1 << 1) | (16 << 1)   # E side
                    if 0 <= col < self.w:
                        mv[y * self.w + col] |= (1 << 3) | (16 << 3)       # W side
        # a diagonal line has no cell edge to lie on: ignored, and rare

    def composite(self):
        """Per cell, the layers below the player as one opaque tile, and the
        layers above it as one tile with transparency (or None)."""
        from PIL import Image
        below, above = [], []
        for i in range(self.w * self.h):
            lo = Image.new("RGBA", (TILE, TILE), (0, 0, 0, 255))
            hi = None
            for n, (_, cells) in enumerate(self.layers):
                c = cells[i]
                if not c:
                    continue
                t = c[0].tile(c[1])
                if n < BELOW_LAYERS:
                    lo.alpha_composite(t)
                else:
                    if hi is None:
                        hi = Image.new("RGBA", (TILE, TILE), (0, 0, 0, 0))
                    hi.alpha_composite(t)
            if hi is not None and hi.getchannel("A").getextrema()[1] < 128:
                hi = None
            below.append(lo)
            above.append(hi)
        return below, above

    def events(self, region_maps, rules, report):
        warps, zones, talks = [], [], []
        dropped = {}
        for o in self.objects:
            if (o.get("type") or "") != "event":
                continue
            p = parse_props(o)
            acts = [p[k] for k in sorted(p, key=self._order) if k.startswith("act")]
            conds = [p[k] for k in sorted(p, key=self._order) if k.startswith("cond")]
            facing = 0xFF
            for c in conds:
                m = re.match(r"is char_facing player,(\w+)", c)
                if m and m.group(1) in DIRS:
                    facing = DIRS[m.group(1)]
            rect = self.cell_rect(o)
            for a in acts:
                verb, _, arg = a.partition(" ")
                args = [s.strip() for s in arg.split(",")]
                if verb == "transition_teleport" and len(args) >= 4:
                    to = Path(args[1]).stem
                    if to not in region_maps:
                        dropped["warp out of the region"] = dropped.get("warp out of the region", 0) + 1
                        continue
                    warps.append((rect, to, int(float(args[2])), int(float(args[3])), facing))
                elif verb == "random_encounter":
                    slug = args[0]
                    if slug not in rules.encounters:
                        dropped[f"encounter {slug} (no table in the region)"] = \
                            dropped.get(f"encounter {slug} (no table in the region)", 0) + 1
                        continue
                    # Tuxemon starts a battle on a step with total_prob % (1 by default)
                    chance = float(args[1]) if len(args) > 1 and args[1] else 1.0
                    if not 0 < chance <= 100:
                        fail(f"{self.slug}: encounter chance {chance} %")
                    zones.append((rect, slug, round(chance * 100)))
                elif verb == "translated_dialog":
                    s = rules.po.get(args[0])
                    if not s:
                        dropped[f"dialog {args[0]} (no English text)"] = 1
                        continue
                    talks.append((rect, facing, text_ok(s.replace("\\n", " "), f"{self.slug} {args[0]}")))
                else:
                    dropped[verb] = dropped.get(verb, 0) + 1
        if dropped:
            report.append(f"{self.slug}: events not taken: " +
                          ", ".join(f"{k} x{v}" for k, v in sorted(dropped.items())))
        return warps, zones, talks

    @staticmethod
    def _order(k):
        m = re.match(r"[a-z]+(\d+)", k)
        return (int(m.group(1)) if m else 0, k)


# ---------------------------------------------------------------- colours

def rgba_pixels(im):
    b = im.convert("RGBA").tobytes()
    return [tuple(b[i:i + 4]) for i in range(0, len(b), 4)]


def nearest(pal, c, memo):
    v = memo.get(c)
    if v is None:
        r, g, b = c
        v = min(range(len(pal)), key=lambda i: ((pal[i][0] - r) ** 2 * 3 + (pal[i][1] - g) ** 2 * 4 +
                                                (pal[i][2] - b) ** 2 * 2, i))
        memo[c] = v
    return v


def make_palette(images, colours):
    """`colours` opaque colours for every pixel with alpha >= 128 in `images`
    (index 0 is kept for transparency), as RGB565-exact triples. Median cut
    over every pixel; counting each distinct colour once instead was tried in
    G0 and banded the ground (GAME_FORMAT.md section 4)."""
    from PIL import Image
    seen = {}
    px = []
    for im in images:
        for r, g, b, a in rgba_pixels(im):
            if a >= 128:
                c = rgb888(rgb565((r, g, b)))
                px.append(c)
                seen[c] = seen.get(c, 0) + 1
    if not px:
        return [(0, 0, 0)]
    if len(seen) <= colours:
        return sorted(seen)
    strip = Image.new("RGB", (len(px), 1))
    strip.putdata(px)
    q = strip.quantize(colors=colours, method=Image.Quantize.MEDIANCUT, kmeans=4,
                       dither=Image.Dither.NONE)
    flat = q.getpalette()[:colours * 3]
    pal = {rgb888(rgb565(tuple(flat[i:i + 3]))) for i in range(0, len(flat), 3)}
    return sorted(pal)


def index_image(im, pal, memo):
    """Pixel indices: 0 transparent, 1 + palette position otherwise."""
    out = []
    for r, g, b, a in rgba_pixels(im):
        out.append(0 if a < 128 else 1 + nearest(pal, rgb888(rgb565((r, g, b))), memo))
    return out


def pack_pixels(idx, bpp):
    if bpp == 8:
        return bytes(idx)
    return bytes((idx[i] << 4) | idx[i + 1] for i in range(0, len(idx), 2))


def unpack_pixels(b, bpp, n):
    if bpp == 8:
        return list(b[:n])
    out = []
    for x in b:
        out += [x >> 4, x & 15]
    return out[:n]


def palette_bytes(pal, bpp):
    entries = [0] + [rgb565(c) for c in pal]
    entries += [0] * ((1 << bpp) - len(entries))
    return struct.pack(f"<{1 << bpp}H", *entries)


# ---------------------------------------------------------------- writing

class Text:
    def __init__(self):
        self.b = bytearray()
        self.memo = {}

    def ref(self, s):
        """An 8-byte string reference: u32 offset, u16 length, u16 0."""
        e = s.encode("utf-8")
        if len(e) > 65535:
            fail(f"text over 65535 bytes: {s[:40]!r}...")
        if e not in self.memo:
            self.memo[e] = len(self.b)
            self.b += e
        return struct.pack("<IHH", self.memo[e], len(e), 0)


def meta_entry(key, value):
    return struct.pack("<HH", key, len(value)) + value


def build(root, bpp, report, ids, allow_path=ALLOW, region_path=REGION, spike=True,
          source=f"Tuxemon {TUXEMON_COMMIT}"):
    """`source` names where the content came from: the pinned commit, or, for
    the test fixture (--unpinned), what the caller says."""
    from PIL import Image
    allow = load_allow(allow_path)
    region = json.loads(Path(region_path).read_text(encoding="utf-8"))
    if not spike:
        bad = [f"{k} {s}" for k, d in allow.items() for s, v in d.items() if v["status"] == "unverified"]
        if bad:
            fail("unverified allow-list lines: " + ", ".join(bad))
    rules = Rules(root, allow, region, report)
    text = Text()

    # types, shapes, statuses
    nt = len(rules.types)
    type_ix = {t: i for i, t in enumerate(rules.types)}
    TYPE = struct.pack("<HH", nt, 0) + b"".join(text.ref(text_ok(rules.name(t), f"type {t}"))
                                                 for t in rules.types)
    TYPE += bytes(v for row in rules.type_table for v in row)
    SHAP = b"".join(struct.pack("<6B2x", *(s["attributes"][k] for k in
                                            ("hp", "armour", "dodge", "melee", "ranged", "speed")))
                    for s in rules.shapes)
    stat_slugs = sorted(rules.statuses)
    stat_ix = {s: i for i, s in enumerate(stat_slugs)}
    STAT = b""
    for s in stat_slugs:
        d = rules.statuses[s]
        eff = d["effects"][0]
        kind = STATUS_EFFECT.get(eff["type"])
        if kind is None:
            fail(f"status {s}: effect {eff['type']}")
        div = int(eff["parameters"][0])
        immune = 0
        for m in d.get("modifiers", []):
            if m.get("attribute") == "type" and m.get("multiplier") == 0.0:
                for v in m["values"]:
                    immune |= 1 << type_ix[v]
        flags = (1 if d.get("category") == "negative" else 0) | \
                (2 if (d.get("behaviors") or {}).get("persists_after_combat") else 0)
        STAT += struct.pack("<IBBBBH2x", assign_id(ids, "status", s), kind, div, flags, 0, immune)
        STAT += text.ref(text_ok(rules.name(s), f"status {s}")) + b"\0" * 4

    # moves
    tech_slugs = sorted(rules.techs)
    tech_ix = {t: i for i, t in enumerate(tech_slugs)}
    MOVE = b""
    for t in tech_slugs:
        d = rules.techs[t]
        ty = [type_ix[x] for x in d.get("types", [])][:2] + [0xFF, 0xFF]
        effs = []
        for e in d["effects"]:
            p = e.get("parameters") or []
            if e["type"] == "damage":
                effs.append((E_DAMAGE, 0, T_ENEMY))
            elif e["type"] == "healing":
                effs.append((E_HEALING, 0, T_ENEMY if p == ["enemy_monster"] else T_OWN))
            else:
                effs.append((E_GIVE, stat_ix[p[0]], T_OWN if p[1] == "own_monster" else T_ENEMY))
        if len(effs) > 3:
            fail(f"move {t}: more than 3 effects")
        effs += [(0, 0, 0)] * (3 - len(effs))
        tgt = d.get("target") or {}
        tmask = sum(1 << i for i, k in enumerate(("enemy_monster", "own_monster")) if tgt.get(k))
        power = round(float(d.get("power", 0)) * 100)
        heal = round(float(d.get("healing_power", 0)) * 100)
        acc = round(float(d.get("accuracy", 0)) * 100)
        pot = round(float(d.get("potency", 0)) * 100)
        if not (0 <= power <= 65535 and 0 <= heal <= 65535 and 0 <= acc <= 100 and 0 <= pot <= 100):
            fail(f"move {t}: a value out of range")
        MOVE += struct.pack("<IBBBBHBBBBH", assign_id(ids, "move", t), ty[0], ty[1],
                            RANGES.index(d["range"]), SPEEDS.index(d.get("speed", "normal")),
                            power, acc, pot, int(d.get("recharge", 0)), tmask, heal)
        MOVE += b"".join(struct.pack("<BBBx", *e) for e in effs)
        MOVE += b"\0" * 4
        MOVE += text.ref(text_ok(rules.name(t), f"move {t}"))

    # creatures, learnsets, sprites
    mon_slugs = sorted(rules.mons, key=lambda s: (rules.mons[s].get("txmn_id", 9999), s))
    mon_ix = {s: i for i, s in enumerate(mon_slugs)}
    MONS, LRNS, SPRT = b"", b"", bytearray()
    sprite_report = []
    for s in mon_slugs:
        d = rules.mons[s]
        ty = [type_ix[x] for x in d["types"]][:2] + [0xFF, 0xFF]
        learn = sorted(((mv["level_learned"], tech_ix[mv["technique"]]) for mv in d["moveset"]
                        if mv["technique"] in tech_ix), key=lambda x: (x[0], tech_slugs[x[1]]))
        if not any(lv <= 1 for lv, _ in learn):
            fail(f"creature {s}: no move it can use at level 1 once unsupported ones are left out")
        ev_lvl, ev_to = 0, 0xFFFF
        for e in d.get("evolutions", []):
            if e.get("at_level") and e.get("monster_slug") in mon_ix:
                ev_lvl, ev_to = int(e["at_level"]), mon_ix[e["monster_slug"]]
                break
            elif e.get("monster_slug"):
                report.append(f"creature {s}: evolution to {e['monster_slug']} left out "
                              f"({'not allow-listed' if e.get('monster_slug') not in mon_ix else 'no level'})")
        grp = d.get("experience_group", "default")
        cr = round(float(d.get("catch_rate", 0)))
        lo = round(float(d.get("lower_catch_resistance", 1)) * 100)
        hi = round(float(d.get("upper_catch_resistance", 1)) * 100)
        # sprite: front at (0,0), back at (64,0) of the 128x88 sheet
        sheet = Image.open(root / "mods" / "tuxemon" / "gfx" / "sprites" / "battle" / f"{s}-sheet.png").convert("RGBA")
        front = sheet.crop((0, 0, SPRITE_W, SPRITE_H))
        back = sheet.crop((SPRITE_W, 0, 2 * SPRITE_W, SPRITE_H))
        pal = make_palette([front, back], 15)
        memo = {}
        body = palette_bytes(pal, 4) + pack_pixels(index_image(front, pal, memo), 4) + \
            pack_pixels(index_image(back, pal, memo), 4)
        src_colours = len({c[:3] for im in (front, back) for c in rgba_pixels(im) if c[3] >= 128})
        sprite_report.append((s, src_colours))
        rec = struct.pack("<BBBBI", SPRITE_W, SPRITE_H, 4, 2, crc32(body)) + body
        sp_off = len(SPRT)
        SPRT += pad4(rec)
        MONS += struct.pack("<IHBBBBBBBBBBHHIII", assign_id(ids, "creature", s), int(d.get("txmn_id", 0)),
                            ty[0], ty[1], rules.shape_ix[d["shape"]], STAGES.index(d["stage"]), cr,
                            EXP_GROUPS.index(grp), lo, hi, ev_lvl, len(learn), ev_to, 0,
                            len(LRNS) // 4, sp_off, len(rec))
        MONS += text.ref(text_ok(rules.name(s), f"creature {s}")) + b"\0" * 8
        LRNS += b"".join(struct.pack("<BxH", lv, m) for lv, m in learn)
    reduced = sum(1 for _, c in sprite_report if c > 15)
    report.append(f"battle sprites: {len(sprite_report)}, {reduced} reduced to 15 colours "
                  f"(sources: {', '.join(f'{s} {c}' for s, c in sprite_report)})")

    # encounter tables
    enc_slugs = sorted(rules.encounters)
    enc_ix = {e: i for i, e in enumerate(enc_slugs)}
    ENCT = struct.pack("<HH", len(enc_slugs), 0)
    rows = b""
    first = 0
    for e in enc_slugs:
        ENCT += struct.pack("<HH", first, len(rules.encounters[e]))
        for r in rules.encounters[e]:
            rows += struct.pack("<HBBH2x", mon_ix[r["creature"]], r["levels"][0], r["levels"][1],
                                round(float(r["rate"]) * 100))
            first += 1
    ENCT = pad4(ENCT) + rows

    # areas
    region_maps = [a["map"] for a in region["areas"]]
    area_ix = {m: i for i, m in enumerate(region_maps)}
    allowed = set(allow["tileset"])
    cache = {}
    AREA, ADAT = b"", bytearray()
    chars = [region["player"]]
    for c in chars:
        if c not in allow["character"]:
            fail(f"game_region.json: character {c} isn't in the allow-list")
    char_imgs = [Image.open(root / "mods" / "tuxemon" / "sprites" / f"{c}.png").convert("RGBA") for c in chars]
    for a in region["areas"]:
        ar = Area(root, a["map"], allowed, cache, report)
        mv, hop = ar.movement()
        below, above = ar.composite()
        tiles, tix = [], {}

        def tile_id(im):
            k = im.tobytes()
            if k not in tix:
                tix[k] = len(tiles) + 1          # tile 0 = nothing
                tiles.append(im)
            return tix[k]
        lo_ids = [tile_id(t) for t in below]
        overlays = [(i, tile_id(t)) for i, t in enumerate(above) if t is not None]
        if len(tiles) > 4095:
            fail(f"{ar.slug}: {len(tiles)} different tiles; the limit is 4095")
        frames = []
        for im in char_imgs:
            for f in range(CHAR_FRAMES):
                x, y = f % 3 * CHAR_W, f // 3 * CHAR_H
                frames.append(im.crop((x, y, x + CHAR_W, y + CHAR_H)))
        pal = make_palette(tiles + frames, (1 << bpp) - 1)
        memo = {}
        tile_bytes = b"".join(pack_pixels(index_image(t, pal, memo), bpp) for t in tiles)
        char_bytes = b"".join(pack_pixels(index_image(f, pal, memo), bpp) for f in frames)
        tflags = bytearray(len(tiles) + 1)
        for i in hop:
            tflags[lo_ids[i]] |= 1
        warps, zones, talks = ar.events(region_maps, rules, report)
        ccols, crows = -(-ar.w // CHUNK), -(-ar.h // CHUNK)
        chunks = []
        for cy in range(crows):
            for cx in range(ccols):
                t, m = [], []
                for yy in range(cy * CHUNK, cy * CHUNK + CHUNK):
                    for xx in range(cx * CHUNK, cx * CHUNK + CHUNK):
                        if xx < ar.w and yy < ar.h:
                            t.append(lo_ids[yy * ar.w + xx])
                            m.append(mv[yy * ar.w + xx])
                        else:
                            t.append(0)
                            m.append(0x0F)
                chunks.append(struct.pack(f"<{CHUNK * CHUNK}H", *t) + bytes(m))
        # the head: header, palette, tile flags, warps, zones, talks, overlays, chunk directory
        pal_b = pad4(struct.pack(f"<{1 + len(pal)}H", 0, *(rgb565(c) for c in pal)))
        body = pal_b + pad4(bytes(tflags))
        for (x0, y0, x1, y1), to, tx, ty_, fc in warps:
            body += struct.pack("<BBBBHBBBxH", x0, y0, x1 - x0, y1 - y0, area_ix[to], tx, ty_, fc, 0)
        for (x0, y0, x1, y1), e, chance in zones:
            body += struct.pack("<BBBBHH", x0, y0, x1 - x0, y1 - y0, enc_ix[e], chance)
        for (x0, y0, x1, y1), fc, s in talks:
            body += struct.pack("<BBBBBxH", x0, y0, x1 - x0, y1 - y0, fc, 0) + text.ref(s)
        for i, t in overlays:
            body += struct.pack("<HH", i, t)
        head_len = 64 + len(body) + 8 * len(chunks)
        tiles_off = (head_len + 3) & ~3
        chars_off = tiles_off + len(tile_bytes)
        chunk0 = (chars_off + len(char_bytes) + 3) & ~3
        cdir = b""
        off = chunk0
        for c in chunks:
            cdir += struct.pack("<II", off, crc32(c))
            off += len(c)
        hdr = struct.pack("<HHBBHHHHHHHHHIIIIIIII", ar.w, ar.h, bpp, len(pal), len(tiles), ccols, crows,
                          len(warps), len(zones), len(talks), len(overlays), len(chars), 0,
                          tiles_off, len(tile_bytes), crc32(tile_bytes),
                          chars_off, len(char_bytes), crc32(char_bytes), chunk0, 0)
        hdr += b"\0" * (64 - len(hdr))
        head = hdr + body + cdir
        block = pad4(head) + tile_bytes + char_bytes
        block = pad4(block) + b"".join(chunks)
        assert len(pad4(head)) == tiles_off and len(pad4(head) + tile_bytes + char_bytes) <= chunk0
        # the most different tiles one screen (plus the row and column scrolling in) can show
        worst = 0
        ov = dict(overlays)
        for y0 in range(max(1, ar.h - VIEW_H)):
            for x0 in range(max(1, ar.w - VIEW_W)):
                seen = set()
                for yy in range(y0, min(ar.h, y0 + VIEW_H + 1)):
                    for xx in range(x0, min(ar.w, x0 + VIEW_W + 1)):
                        i = yy * ar.w + xx
                        seen.add(lo_ids[i])
                        if i in ov:
                            seen.add(ov[i])
                worst = max(worst, len(seen))
        report.append(f"{ar.slug}: {ar.w}x{ar.h} cells, {len(tiles)} tiles, {len(overlays)} overlays, "
                      f"{len(warps)} warps, {len(zones)} zones, {len(talks)} talks; "
                      f"{len(pal)} colours; head {len(head)} B, tiles {len(tile_bytes)} B, "
                      f"characters {len(char_bytes)} B, {len(chunks)} chunks of {len(chunks[0])} B; "
                      f"at most {worst} tiles on one screen")
        AREA += struct.pack("<IHHIII", assign_id(ids, "area", ar.slug), ar.w, ar.h, len(ADAT),
                            len(block), crc32(head)) + text.ref(text_ok(a.get("name", ar.slug), ar.slug)) + \
            struct.pack("<I", len(head))
        ADAT += pad4(block)
        st = region["start"]
        if st["area"] == ar.slug:
            if not (0 <= st["x"] < ar.w and 0 <= st["y"] < ar.h) or mv[st["y"] * ar.w + st["x"]] & 0x0F == 0x0F:
                fail(f"game_region.json: the start {st['x']},{st['y']} isn't a cell one can stand on")

    # META
    st = region["start"]
    if st["area"] not in area_ix:
        fail(f"game_region.json: the start area {st['area']} isn't in the region")
    META = meta_entry(K_TITLE, text_ok(region["title"], "title").encode())
    META += meta_entry(K_SOURCE, source.encode())
    META += meta_entry(K_BUILD, f"mkgame {FORMAT_MAJOR}.{FORMAT_MINOR}".encode())
    META += meta_entry(K_START, struct.pack("<HBBB3x", area_ix[st["area"]], st["x"], st["y"],
                                            DIRS[st.get("facing", "down")]))
    mc, cc, cb = rules.monster_cfg, rules.capture, rules.combat
    lo, hi = cb["multiplier_range"]
    META += meta_entry(K_RULES, struct.pack("<8I", 7, int(mc["coeff_stats"]), round(lo * 100), round(hi * 100),
                                            int(mc["experience_groups"]["default"]["experience_coefficient"]),
                                            int(mc["level_range"][1]), int(mc["max_moves"]), 6))
    META += meta_entry(K_CAPTURE, struct.pack("<7I", cc["total_shakes"], cc["shake_constant"],
                                              cc["shake_denominator"], cc["shake_divisor"],
                                              cc["shake_hp_multiplier"], cc["shake_current_hp_multiplier"],
                                              cc["shake_hp_divisor"]))
    eg = mc["experience_groups"]
    META += meta_entry(K_GROWTH, struct.pack("<4I", int(mc["iv_range"][1]),
                                             *(round(float(eg[g]["multiplier"]) * 100) for g in EXP_GROUPS)))

    # credits
    if source.startswith("Tuxemon "):
        cred = [f"Game data from Tuxemon (GPLv3), commit {source[8:20]}.", "github.com/Tuxemon/Tuxemon", ""]
    else:
        cred = [f"Game data from {source}.", ""]
    for kind, title in (("creature", "Creatures"), ("tileset", "Tiles"), ("character", "Characters")):
        cred.append(title)
        for s in sorted(allow[kind]):
            v = allow[kind][s]
            nm = rules.name(s) if kind == "creature" else s
            cred.append(f"{nm}: {v['authors'].replace('; ', ', ')} ({v['licence']})")
        cred.append("")
    CRED = "\n".join(text_ok(c, "credits") if c else "" for c in cred).encode("utf-8")

    secs = {b"META": META, b"TYPE": TYPE, b"SHAP": SHAP, b"STAT": STAT, b"MOVE": MOVE, b"MONS": MONS,
            b"LRNS": LRNS, b"ENCT": ENCT, b"AREA": AREA, b"ADAT": bytes(ADAT), b"SPRT": bytes(SPRT),
            b"TEXT": bytes(text.b), b"CRED": CRED}
    off = 32 + 16 * len(SECTIONS)
    table, body = b"", b""
    for tag in SECTIONS:
        b = secs[tag]
        table += tag + struct.pack("<III", off + len(body), len(b), crc32(b))
        body += pad4(b)
    total = off + len(body)
    hdr = MAGIC + struct.pack("<HHIHHII", FORMAT_MAJOR, FORMAT_MINOR, total, len(SECTIONS), 0,
                              len(mon_slugs), len(region["areas"])) + struct.pack("<I", crc32(table))
    hdr += struct.pack("<I", crc32(hdr))
    return hdr + table + body


# ---------------------------------------------------------------- reading

class Game:
    """An independent reader of game.bin: what --check, --dump and --preview use."""

    def __init__(self, data):
        self.d = d = data
        if len(d) < 32 or d[:4] != MAGIC:
            fail("not a game file")
        major, minor, size, nsec, _, self.nmon, self.narea, tcrc, hcrc = struct.unpack_from("<HHIHHIIII", d, 4)
        if crc32(d[:28]) != hcrc:
            fail("header CRC")
        if major != FORMAT_MAJOR:
            fail(f"format {major}.{minor}; this reader is {FORMAT_MAJOR}.x")
        if size != len(d):
            fail(f"file is {len(d)} B, header says {size}")
        table = d[32:32 + 16 * nsec]
        if len(table) != 16 * nsec or crc32(table) != tcrc:
            fail("section table CRC")
        self.sec = {}
        prev = 32 + 16 * nsec
        for i in range(nsec):
            tag = table[16 * i:16 * i + 4]
            off, ln, c = struct.unpack_from("<III", table, 16 * i + 4)
            if off % 4 or off < prev or off + ln > len(d):
                fail(f"section {tag!r} out of place")
            if any(d[prev:off]):
                fail(f"non-zero padding before section {tag!r}")
            prev = off + ln
            b = d[off:off + ln]
            if crc32(b) != c:
                fail(f"section {tag.decode()} CRC")
            self.sec[tag] = b
        for t in SECTIONS:
            if t not in self.sec:
                fail(f"no {t.decode()} section")
        self.text = self.sec[b"TEXT"]

    def s(self, b, o):
        off, ln, _ = struct.unpack_from("<IHH", b, o)
        if off + ln > len(self.text):
            fail("string outside TEXT")
        v = self.text[off:off + ln].decode("utf-8")
        if any(not 0x20 <= ord(ch) <= 0xFF for ch in v):
            fail(f"string with characters outside U+0020-U+00FF: {v!r}")
        return v

    def meta(self):
        m, b, o = {}, self.sec[b"META"], 0
        while o < len(b):
            k, n = struct.unpack_from("<HH", b, o)
            m[k] = b[o + 4:o + 4 + n]
            o += 4 + n
        return m

    def types(self):
        b = self.sec[b"TYPE"]
        n = struct.unpack_from("<H", b)[0]
        names = [self.s(b, 4 + 8 * i) for i in range(n)]
        t0 = 4 + 8 * n
        return names, [list(b[t0 + n * i:t0 + n * i + n]) for i in range(n)]

    def records(self, tag, size):
        b = self.sec[tag]
        if len(b) % size:
            fail(f"{tag.decode()} isn't whole {size}-byte records")
        return [b[i:i + size] for i in range(0, len(b), size)]

    def areas(self):
        out = []
        for r in self.records(b"AREA", 32):
            aid, w, h, off, ln, hcrc = struct.unpack_from("<IHHIII", r)
            name = self.s(r, 20)
            head_len = struct.unpack_from("<I", r, 28)[0]
            blk = self.sec[b"ADAT"][off:off + ln]
            if len(blk) != ln or crc32(blk[:head_len]) != hcrc:
                fail(f"area {aid}: head CRC")
            out.append(self.area(aid, name, blk, head_len))
        return out

    def area(self, aid, name, blk, head_len):
        (w, h, bpp, ncol, nt, ccols, crows, nw, nz, ntk, nov, nch, _, toff, tlen, tcrc,
         coff, clen, ccrc, chunk0, _) = struct.unpack_from("<HHBBHHHHHHHHHIIIIIIII", blk)
        if bpp not in (4, 8):
            fail(f"area {aid}: {bpp} bits per pixel")
        a = {"id": aid, "name": name, "w": w, "h": h, "bpp": bpp, "ntiles": nt}
        o = 64
        if not 1 <= ncol < 1 << bpp:
            fail(f"area {aid}: {ncol} colours at {bpp} bits per pixel")
        npal = 1 + ncol
        pal = struct.unpack_from(f"<{npal}H", blk, o)
        # the device's lookup table has every index; ones past the colours are black
        a["palette"] = [rgb888(v) for v in pal] + [(0, 0, 0)] * ((1 << bpp) - npal)
        a["colours"] = ncol
        o += (2 * npal + 3) & ~3
        a["tflags"] = list(blk[o:o + nt + 1])
        o += (nt + 1 + 3) & ~3
        a["warps"] = [struct.unpack_from("<BBBBHBBBxH", blk, o + 12 * i)[:8] for i in range(nw)]
        o += 12 * nw
        a["zones"] = [struct.unpack_from("<BBBBHH", blk, o + 8 * i) for i in range(nz)]
        o += 8 * nz
        a["talks"] = [struct.unpack_from("<BBBBB", blk, o + 16 * i) + (self.s(blk, o + 16 * i + 8),)
                      for i in range(ntk)]
        o += 16 * ntk
        a["overlays"] = dict(struct.unpack_from("<HH", blk, o + 4 * i) for i in range(nov))
        o += 4 * nov
        cdir = [struct.unpack_from("<II", blk, o + 8 * i) for i in range(ccols * crows)]
        o += 8 * len(cdir)
        if o != head_len:
            fail(f"area {aid}: head is {head_len} B, contents {o} B")
        tb = blk[toff:toff + tlen]
        if crc32(tb) != tcrc or tlen != nt * TILE * TILE * bpp // 8:
            fail(f"area {aid}: tiles")
        cb = blk[coff:coff + clen]
        if crc32(cb) != ccrc:
            fail(f"area {aid}: character sheets")
        tsz = TILE * TILE * bpp // 8
        a["tiles"] = [None] + [unpack_pixels(tb[i * tsz:(i + 1) * tsz], bpp, TILE * TILE) for i in range(nt)]
        fsz = CHAR_W * CHAR_H * bpp // 8
        a["frames"] = [unpack_pixels(cb[i * fsz:(i + 1) * fsz], bpp, CHAR_W * CHAR_H)
                       for i in range(clen // fsz)]
        csz = CHUNK * CHUNK * 3
        cells, mv = [0] * (w * h), [0] * (w * h)
        for ci, (coff2, ccrc2) in enumerate(cdir):
            c = blk[coff2:coff2 + csz]
            if len(c) != csz or crc32(c) != ccrc2:
                fail(f"area {aid}: chunk {ci}")
            cx, cy = ci % ccols, ci // ccols
            t = struct.unpack_from(f"<{CHUNK * CHUNK}H", c)
            for j in range(CHUNK * CHUNK):
                xx, yy = cx * CHUNK + j % CHUNK, cy * CHUNK + j // CHUNK
                if xx < w and yy < h:
                    if not 0 <= t[j] <= nt:
                        fail(f"area {aid}: cell tile {t[j]}")
                    cells[yy * w + xx] = t[j]
                    mv[yy * w + xx] = c[2 * CHUNK * CHUNK + j]
        a["cells"], a["move"] = cells, mv
        a["sizes"] = {"head": head_len, "tiles": tlen, "characters": clen, "chunks": len(cdir) * csz}
        return a

    def sprites(self):
        out = []
        for r in self.records(b"MONS", 48):
            off, ln = struct.unpack_from("<II", r, 24)
            b = self.sec[b"SPRT"][off:off + ln]
            w, h, bpp, nf, c = struct.unpack_from("<BBBBI", b)
            body = b[8:]
            if crc32(body) != c:
                fail("sprite CRC")
            pal = [rgb888(v) for v in struct.unpack_from(f"<{1 << bpp}H", body)]
            px = body[2 << bpp:]
            fsz = w * h * bpp // 8
            out.append((self.s(r, 32), w, h, pal, [unpack_pixels(px[i * fsz:(i + 1) * fsz], bpp, w * h)
                                                    for i in range(nf)]))
        return out


def dump(g):
    """A canonical text dump of everything in the file: G1's C reader must
    print the same, byte for byte (the round trip, as with courses)."""
    out = []
    m = g.meta()
    out.append(f"title {m[K_TITLE].decode()}")
    out.append(f"source {m[K_SOURCE].decode()}")
    a, x, y, f = struct.unpack_from("<HBBB", m[K_START])
    out.append(f"start area {a} at {x},{y} facing {f}")
    out.append("rules " + " ".join(map(str, struct.unpack("<8I", m[K_RULES]))))
    out.append("capture " + " ".join(map(str, struct.unpack("<7I", m[K_CAPTURE]))))
    out.append("growth " + " ".join(map(str, struct.unpack("<4I", m[K_GROWTH]))))
    names, table = g.types()
    for n, row in zip(names, table):
        out.append(f"type {n} " + " ".join(map(str, row)))
    for i, r in enumerate(g.records(b"SHAP", 8)):
        out.append(f"shape {i} " + " ".join(map(str, r[:6])))
    for r in g.records(b"STAT", 24):
        sid, kind, div, flags, _, imm = struct.unpack_from("<IBBBBH", r)
        out.append(f"status {sid} {g.s(r, 12)} kind {kind} divisor {div} flags {flags} immune {imm:#06x}")
    for r in g.records(b"MOVE", 40):
        v = struct.unpack_from("<IBBBBHBBBBH", r)
        effs = [struct.unpack_from("<BBB", r, 16 + 4 * i) for i in range(3)]
        out.append(f"move {v[0]} {g.s(r, 32)} types {v[1]},{v[2]} range {RANGES[v[3]]} speed {v[4]} "
                   f"power {v[5]} acc {v[6]} pot {v[7]} recharge {v[8]} target {v[9]} heal {v[10]} "
                   f"effects {' '.join('%d/%d/%d' % e for e in effs if e[0])}")
    lr = g.records(b"LRNS", 4)
    for r in g.records(b"MONS", 48):
        v = struct.unpack_from("<IHBBBBBBBBBBHHIII", r)
        learn = " ".join(f"{x[0]}:{struct.unpack_from('<H', x, 2)[0]}" for x in lr[v[14]:v[14] + v[11]])
        out.append(f"creature {v[0]} {g.s(r, 32)} txmn {v[1]} types {v[2]},{v[3]} shape {v[4]} stage {v[5]} "
                   f"catch {v[6]} group {v[7]} resist {v[8]}-{v[9]} evolve {v[10]}->{v[12]} learns {learn}")
    b = g.sec[b"ENCT"]
    n = struct.unpack_from("<H", b)[0]
    rows0 = (4 + 4 * n + 3) & ~3
    for i in range(n):
        first, cnt = struct.unpack_from("<HH", b, 4 + 4 * i)
        rows = [struct.unpack_from("<HBBH", b, rows0 + 8 * j) for j in range(first, first + cnt)]
        out.append(f"encounter {i} " + " ".join(f"{c}:{lo}-{hi}@{r}" for c, lo, hi, r in rows))
    for a in g.areas():
        out.append(f"area {a['id']} {a['name']} {a['w']}x{a['h']} bpp {a['bpp']} tiles {a['ntiles']} "
                   f"palette {' '.join('%02x%02x%02x' % c for c in a['palette'][1:1 + a['colours']])}")
        for wp in a["warps"]:
            out.append(f"  warp {wp[0]},{wp[1]} {wp[2]}x{wp[3]} -> area {wp[4]} {wp[5]},{wp[6]} facing {wp[7]}")
        for z in a["zones"]:
            out.append(f"  zone {z[0]},{z[1]} {z[2]}x{z[3]} encounter {z[4]} chance {z[5]}")
        for t in a["talks"]:
            out.append(f"  talk {t[0]},{t[1]} {t[2]}x{t[3]} facing {t[4]}: {t[5]}")
        out.append(f"  cells crc {crc32(struct.pack(f'<{len(a['cells'])}H', *a['cells'])):08x} "
                   f"move crc {crc32(bytes(a['move'])):08x} overlays {len(a['overlays'])}")
    out.append("credits " + f"{crc32(g.sec[b'CRED']):08x}")
    return "\n".join(out) + "\n"


def sizes(g):
    out = [f"{len(g.d)} B in all"]
    for t in SECTIONS:
        out.append(f"  {t.decode():4} {len(g.sec[t]):8} B")
    for a in g.areas():
        out.append(f"  area {a['name']}: " + ", ".join(f"{k} {v} B" for k, v in a["sizes"].items()))
    return "\n".join(out)


# ---------------------------------------------------------------- previews

def draw_area(a, scale=1):
    """The whole area as the device would draw it: cells, then the player,
    then the overlays; colours through RGB565."""
    from PIL import Image
    pal = a["palette"]
    im = Image.new("RGB", (a["w"] * TILE, a["h"] * TILE))
    px = im.load()

    def put(tile, ox, oy, transparent):
        for j, v in enumerate(tile):
            if v or not transparent:
                px[ox + j % TILE, oy + j // TILE] = pal[v] if v else (0, 0, 0)
    for i, t in enumerate(a["cells"]):
        if t:
            put(a["tiles"][t], i % a["w"] * TILE, i // a["w"] * TILE, False)
    return im, put


def preview(g, outdir, tag, start=None):
    from PIL import Image
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    m = g.meta()
    sa, sx, sy, _ = struct.unpack_from("<HBBB", m[K_START])
    made = []
    for ai, a in enumerate(g.areas()):
        im, put = draw_area(a)
        px = im.load()
        # the player at the start (frame 0, facing down), then the overlays
        if a["frames"] and ai == sa:
            fr = a["frames"][0]
            ox, oy = sx * TILE, sy * TILE - (CHAR_H - TILE)
            for j, v in enumerate(fr):
                if v:
                    px[ox + j % CHAR_W, oy + j // CHAR_W] = a["palette"][v]
        for i, t in a["overlays"].items():
            put(a["tiles"][t], i % a["w"] * TILE, i // a["w"] * TILE, True)
        p = outdir / f"{tag}_{ai}_{a['name'].replace(' ', '_').lower()}.png"
        im.save(p)
        made.append(p)
        # the game view (15 x 11 cells) around the start, or the area's middle
        cx, cy = (sx, sy) if ai == sa else (a["w"] // 2, a["h"] // 2)
        x0 = max(0, min(a["w"] - VIEW_W, cx - VIEW_W // 2)) * TILE
        y0 = max(0, min(a["h"] - VIEW_H, cy - VIEW_H // 2)) * TILE
        v = im.crop((x0, y0, x0 + VIEW_W * TILE, y0 + VIEW_H * TILE))
        p = outdir / f"{tag}_{ai}_view.png"
        v.save(p)
        made.append(p)
    sp = g.sprites()
    cols = 10
    sheet = Image.new("RGB", (cols * 2 * SPRITE_W, -(-len(sp) // cols) * SPRITE_H), (255, 255, 255))
    for k, (name, w, h, pal, frames) in enumerate(sp):
        for f, fr in enumerate(frames):
            ox, oy = (k % cols) * 2 * w + f * w, (k // cols) * h
            for j, v in enumerate(fr):
                if v:
                    sheet.putpixel((ox + j % w, oy + j // w), pal[v])
    p = outdir / f"{tag}_sprites.png"
    sheet.save(p)
    made.append(p)
    return made


# ---------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="*", help="game.bin files for --check, --dump, --preview")
    ap.add_argument("-o", "--output", help="build game.bin here")
    ap.add_argument("--tuxemon", default=str(CACHE), help=f"Tuxemon checkout (default {CACHE})")
    ap.add_argument("--fetch", action="store_true", help="clone Tuxemon at the pinned commit")
    ap.add_argument("--bpp", type=int, choices=(4, 8), default=8,
                    help="map tile depth: 8 (exact, the default) or 4 (GAME_FORMAT.md section 4)")
    ap.add_argument("--allow", default=str(ALLOW))
    ap.add_argument("--region", default=str(REGION))
    ap.add_argument("--ids", default=str(IDS))
    ap.add_argument("--check", action="store_true", help="validate files and print sizes")
    ap.add_argument("--dump", action="store_true", help="print the canonical dump")
    ap.add_argument("--preview", metavar="DIR", help="draw each file's areas and sprites into DIR")
    ap.add_argument("--report", action="store_true", help="print the build report")
    ap.add_argument("--unpinned", metavar="SOURCE",
                    help="build from a tree that isn't the pinned checkout, naming it SOURCE "
                         "(the test fixture, tests/data/game/; never for a real build)")
    ap.add_argument("--strict", action="store_true",
                    help="refuse allow-list lines that are still unverified (G3's build)")
    args = ap.parse_args(argv)
    try:
        if args.fetch:
            fetch(args.tuxemon)
            print(f"Tuxemon {TUXEMON_COMMIT[:12]} in {args.tuxemon}")
        if args.output:
            if args.unpinned:
                root, source = Path(args.tuxemon), args.unpinned
                if not (root / "mods" / "tuxemon" / "db").is_dir():
                    fail(f"{root} isn't a Tuxemon-shaped tree")
            else:
                root, source = check_source(args.tuxemon), f"Tuxemon {TUXEMON_COMMIT}"
            report = []
            ids = load_ids(args.ids)
            data = build(root, args.bpp, report, ids, args.allow, args.region, spike=not args.strict,
                         source=source)
            save_ids(ids, args.ids)
            Path(args.output).write_bytes(data)
            Game(data)
            print(f"{args.output}: {len(data)} B")
            if args.report:
                print("\n".join(report))
        for f in args.files:
            g = Game(Path(f).read_bytes())
            g.areas()
            g.sprites()
            if args.check:
                print(f"{f}: ok\n{sizes(g)}")
            if args.dump:
                sys.stdout.write(dump(g))
            if args.preview:
                for p in preview(g, args.preview, Path(f).stem):
                    print(p)
    except GameError as e:
        print(f"mkgame: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
