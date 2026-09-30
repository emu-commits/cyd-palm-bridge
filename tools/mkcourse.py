#!/usr/bin/env python3
"""Build a Study course file (course.srs) from a course source folder.

    python3 tools/mkcourse.py courses/demo-kanji               # -> courses/demo-kanji/course.srs
    python3 tools/mkcourse.py courses/demo-kanji -o out.srs    # somewhere else
    python3 tools/mkcourse.py --check path/to/course.srs       # validate a built file, print sizes
    python3 tools/mkcourse.py --dump SOURCE_DIR_OR_FILE        # canonical text dump (round-trip gate)

The format is docs/COURSE_FORMAT.md; the device's reader is firmware/main/course.c.
Both must agree with this file, and the round-trip gate (make -C sim course)
proves it: the dump of a source folder (this file's model), the dump of the file
it builds read back by this file's own reader, and the dump written by the C
reader must be identical.

BUILDS ARE REPRODUCIBLE, to the byte: nothing depends on the time, the machine or
dict order; the picture font is pinned by SHA-256, and Pillow by version
(tools/requirements-course.txt), with its layout engine forced to BASIC so an
installed libraqm can't change the shaping. CI rebuilds the demo course and fails
if one byte differs from the committed file.

A picture font is needed only when some text is neither Palm-font text
(U+0020-U+00FF) nor kana: it is downloaded once into ~/.cache/cyd-palm/ and its
hash checked, or given with --font.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import unicodedata
import urllib.request
import zlib
from pathlib import Path

FORMAT_MAJOR, FORMAT_MINOR = 1, 0
MAGIC = b"SRSC"

FONT_NAME = "NotoSansJP-Regular.otf"
FONT_URL = ("https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/"
            "Sans/SubsetOTF/JP/NotoSansJP-Regular.otf")
FONT_SHA256 = "dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073"

# pictures (COURSE_FORMAT.md section 4)
WRAP_W = 232
H_PROMPT, H_WORD, H_TEXT = 64, 40, 24
H_WORD_MIN = 16

# limits (section 5)
MAX_ITEMS = 65535
MAX_LEVELS = 65535
MAX_TEXT = 4096
MAX_LINKS = 64
MAX_KINDS = 16
MAX_ROLES = 15
MAX_GROUPS = 15
MAX_META = 4096
MAX_BMP_W, MAX_BMP_H = 240, 1024

# field tags (section 3.5)
TERM, MEANING, READING, SENSE, EXAMPLE, TRANSLATION = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
MNEM_M, MNEM_R, ETYMOLOGY, NOTE, FORM = 0x07, 0x08, 0x09, 0x0A, 0x0B
LEVEL_TITLE, LEVEL_THEME, LEVEL_GLOSS = 0x20, 0x21, 0x22
TAG_NAMES = {TERM: "TERM", MEANING: "MEANING", READING: "READING", SENSE: "SENSE",
             EXAMPLE: "EXAMPLE", TRANSLATION: "TRANSLATION", MNEM_M: "MNEMONIC_MEANING",
             MNEM_R: "MNEMONIC_READING", ETYMOLOGY: "ETYMOLOGY", NOTE: "NOTE", FORM: "FORM",
             LEVEL_TITLE: "LEVEL_TITLE", LEVEL_THEME: "LEVEL_THEME", LEVEL_GLOSS: "LEVEL_GLOSS"}
A_PICTURE, A_PRIMARY, A_KANA = 0x80, 0x40, 0x20
READING_TYPES = {"word": 0, "on": 1, "kun": 2, "nanori": 3}
STRIPPABLE = {"etymology": ETYMOLOGY, "note": NOTE, "example": EXAMPLE, "translation": TRANSLATION}
LINK_UNLOCK, LINK_BUILT, LINK_FAMILY, LINK_RELATED = 1, 2, 3, 4
LINK_NAMES = {LINK_UNLOCK: "unlock_after", LINK_BUILT: "built_from", LINK_FAMILY: "family",
              LINK_RELATED: "related"}
SEP = "\x1f"

# META keys (section 3.2)
K_ID, K_TITLE, K_VERSION, K_AUTHOR, K_LICENCE, K_LANGUAGE, K_DESC, K_SOURCE = range(1, 9)
K_KIND, K_ROLE, K_GROUP = 0x10, 0x11, 0x12
K_SCHED, K_GRADING, K_BATCH, K_LEVEL_PCT, K_BY_LINKS, K_KNOWN = 0x20, 0x21, 0x22, 0x23, 0x24, 0x25
K_STAGES, K_DROPS = 0x30, 0x31
K_LADDER, K_MULT, K_EASE, K_EASE_CHANGE, K_RELEARN, K_DAY_ALIGNED = 0x40, 0x41, 0x42, 0x43, 0x44, 0x45
K_RENDER = 0x50
TEXT_KEYS = {K_ID, K_TITLE, K_VERSION, K_AUTHOR, K_LICENCE, K_LANGUAGE, K_DESC, K_SOURCE, K_ROLE}
SIGNED_KEYS = {K_EASE_CHANGE}

SECTIONS = [b"META", b"LEVL", b"ITEM", b"TEXT", b"BMP ", b"LINK", b"IDX "]
REQUIRED = {b"META", b"LEVL", b"ITEM", b"TEXT", b"IDX "}


class CourseError(Exception):
    pass


def fail(msg):
    raise CourseError(msg)


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


# ---------------------------------------------------------------- text rules

FOLD = {"‘": "'", "’": "'", "‚": "'", "‛": "'", "′": "'",
        "“": '"', "”": '"', "„": '"', "‟": '"', "″": '"',
        "–": "-", "—": "-", "‒": "-", "−": "-", "‐": "-", "‑": "-",
        "…": "...", " ": " ", " ": " ", " ": " "}


FOLD.update({"\u2044": "/", "\u2215": "/", "\u2192": "->", "\u2190": "<-", "\u2194": "<->",
             "\u21d2": "=>", "\u2070": "0"})
for _i in range(10):
    FOLD[chr(0x2080 + _i)] = str(_i)                  # subscript digits
for _i in range(4, 10):
    FOLD[chr(0x2070 + _i)] = str(_i)                  # superscripts; 1 2 3 are Latin-1
WHITESPACE_RUN = re.compile(r"[ ]*[\t\r\n][\t\r\n ]*")


def fold(s):
    """Typographic characters the Palm font lacks, and line breaks, to plain
    ones: a course's text is one flowing paragraph per field."""
    s = unicodedata.normalize("NFC", s)
    s = WHITESPACE_RUN.sub(" ", s)
    return "".join(FOLD.get(ch, ch) for ch in s)


def latin_fallback(s):
    """A Latin letter outside Latin-1 (č, ā, ſ, ʰ) to the letters it's built
    on (c, a, s, h), as a Palm did it. Not for TERM or READING, which must be
    exact; any other character is left alone."""
    out = []
    for ch in s:
        if 0x20 <= ord(ch) <= 0xFF or ch == SEP:
            out.append(ch)
            continue
        d = "".join(x for x in unicodedata.normalize("NFKD", ch) if not unicodedata.combining(x))
        if d and all(x.isascii() and x.isalpha() for x in d):
            out.append(d)
        else:
            out.append(ch)
    return "".join(out)


def is_palm(s, allow_sep=False):
    return all(0x20 <= ord(ch) <= 0xFF or (allow_sep and ch == SEP) for ch in s)


def is_kana_char(ch):
    o = ord(ch)
    return 0x3041 <= o <= 0x3096 or 0x309B <= o <= 0x309E or 0x30A1 <= o <= 0x30FE


def is_kana(s):
    return bool(s) and all(is_kana_char(ch) for ch in s)


def strip_unpalm(s):
    """Drop the runs the Palm font can't draw; then brackets or quotes left empty,
    and doubled spaces. 'Turkish حال (hal)' -> 'Turkish (hal)'."""
    out = "".join(ch if 0x20 <= ord(ch) <= 0xFF else "\x00" for ch in s)
    while "\x00\x00" in out:
        out = out.replace("\x00\x00", "\x00")
    out = out.replace("\x00", "")
    for pair in ("()", "[]", "\"\"", "''", "( )", "[ ]"):
        out = out.replace(pair, "")
    while "  " in out:
        out = out.replace("  ", " ")
    out = out.replace(" ,", ",").replace(" .", ".").replace("( ", "(").replace(" )", ")")
    return out.strip()


# ---------------------------------------------------------------- pictures

class Painter:
    """Draws the text the device can't, with a pinned font and Pillow version."""

    def __init__(self, font_path):
        self.font_path = font_path
        self.fonts = {}
        self.notdef = {}
        try:
            from PIL import Image, ImageDraw, ImageFont
        except ImportError:
            fail("this course has text that must be drawn as pictures, which needs Pillow: "
                 "pip install -r tools/requirements-course.txt")
        self.Image, self.ImageDraw, self.ImageFont = Image, ImageDraw, ImageFont

    def font(self, size):
        if size not in self.fonts:
            self.fonts[size] = self.ImageFont.truetype(
                str(self.font_path), size, layout_engine=self.ImageFont.Layout.BASIC)
        return self.fonts[size]

    def _mask_bytes(self, ch, size):
        m = self.font(size).getmask(ch)
        return (m.size, bytes(m))

    def check_glyphs(self, s, where):
        size = H_TEXT
        if size not in self.notdef:
            self.notdef[size] = self._mask_bytes("\U0010FFFD", size)
        for ch in set(s):
            if ch == " " or is_palm(ch):
                continue
            if self._mask_bytes(ch, size) == self.notdef[size]:
                fail("%s: the picture font has no glyph for %r (U+%04X)" % (where, ch, ord(ch)))

    def width(self, s, size):
        return self.font(size).getlength(s)

    def wrap(self, s, size, width):
        """Greedy wrap: CJK breaks between any two characters, other scripts at
        spaces; a closing mark never starts a line."""
        tokens, cur = [], ""
        for ch in s:
            wide = unicodedata.east_asian_width(ch) in ("W", "F")
            if ch == " ":
                if cur:
                    tokens.append(cur)
                cur = ""
                tokens.append(" ")
            elif wide:
                if cur:
                    tokens.append(cur)
                cur = ""
                tokens.append(ch)
            else:
                cur += ch
        if cur:
            tokens.append(cur)
        closing = set("、。，．！？）」』】〉》ー…ぁぃぅぇぉっゃゅょァィゥェォッャュョ,.!?)]")
        lines, line = [], []
        for t in tokens:
            trial = "".join(line + [t]).rstrip()
            if line and self.width(trial, size) > width and t != " ":
                carry = []
                if t[0] in closing and len(line) > 1:
                    carry = [line.pop()]
                lines.append("".join(line).strip())
                line = carry + ([t] if t != " " else [])
            elif not (t == " " and not line):
                line.append(t)
        if line:
            lines.append("".join(line).strip())
        for ln in lines:
            if self.width(ln, size) > width:
                fail("a word is wider than the screen: %r" % ln)
        return [ln for ln in lines if ln]

    def render(self, lines, size):
        f = self.font(size)
        pitch = int(size * 1.4)
        w = max(int(self.width(ln, size)) for ln in lines) + size
        h = pitch * len(lines) + size
        img = self.Image.new("L", (w, h), 0)
        d = self.ImageDraw.Draw(img)
        for i, ln in enumerate(lines):
            d.text((0, i * pitch), ln, fill=255, font=f)
        bw = img.point(lambda v: 1 if v >= 128 else 0, mode="1")
        box = bw.getbbox()
        if not box:
            fail("nothing was drawn for %r" % lines)
        bw = bw.crop(box)
        bw_w, bw_h = bw.size
        if bw_w > MAX_BMP_W or bw_h > MAX_BMP_H:
            fail("a picture is %dx%d, over %dx%d: %r" % (bw_w, bw_h, MAX_BMP_W, MAX_BMP_H, lines))
        stride = (bw_w + 7) // 8
        px = bw.load()
        rows = bytearray()
        for y in range(bw_h):
            row = bytearray(stride)
            for x in range(bw_w):
                if px[x, y]:
                    row[x >> 3] |= 0x80 >> (x & 7)
            rows += row
        return bw_w, bw_h, stride, bytes(rows)

    def picture(self, tag, text, where):
        shown = text
        if tag == SENSE:
            parts = text.split(SEP)
            shown = parts[0] + ": " + parts[1] + (" (" + parts[2] + ")" if len(parts) > 2 else "")
        elif tag == FORM:
            t, form = text.split(SEP)
            shown = t + ": " + form
        self.check_glyphs(shown, where)
        if tag == TERM:
            if len(shown) == 1:
                return self.render([shown], H_PROMPT)
            size = H_WORD
            while size > H_WORD_MIN and self.width(shown, size) > WRAP_W:
                size -= 2
            if self.width(shown, size) <= WRAP_W:
                return self.render([shown], size)
            return self.render(self.wrap(shown, H_WORD_MIN, WRAP_W), H_WORD_MIN)
        return self.render(self.wrap(shown, H_TEXT, WRAP_W), H_TEXT)


def font_path(explicit):
    if explicit:
        p = Path(explicit)
    else:
        cache = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "cyd-palm"
        p = cache / FONT_NAME
        if not p.exists():
            cache.mkdir(parents=True, exist_ok=True)
            print("mkcourse: downloading %s ..." % FONT_NAME, file=sys.stderr)
            tmp = p.with_suffix(".part")
            try:
                with urllib.request.urlopen(FONT_URL, timeout=120) as r, open(tmp, "wb") as out:
                    out.write(r.read())
            except OSError as e:
                fail("could not download the picture font (%s). Fetch %s yourself and pass "
                     "--font PATH." % (e, FONT_URL))
            tmp.replace(p)
    if not p.exists():
        fail("no font at %s" % p)
    h = hashlib.sha256(p.read_bytes()).hexdigest()
    if h != FONT_SHA256:
        fail("%s is not the pinned font (sha256 %s, want %s): every build of a course must "
             "draw the same pixels" % (p, h, FONT_SHA256))
    return p


# ---------------------------------------------------------------- the source

def duration(v, where):
    if not isinstance(v, str) or len(v) < 2 or v[-1] not in "mhdw" or not v[:-1].isdigit():
        fail("%s: a duration is a whole number and m, h, d or w, like \"4h\": got %r" % (where, v))
    n = int(v[:-1])
    return n * {"m": 60, "h": 3600, "d": 86400, "w": 604800}[v[-1]]


def hundredths(v, where, signed=False):
    if not isinstance(v, (int, float)) or isinstance(v, bool):
        fail("%s: expected a number, got %r" % (where, v))
    h = round(v * 100)
    if abs(h - v * 100) > 1e-6:
        fail("%s: %r has more than two decimals" % (where, v))
    if not signed and h < 0:
        fail("%s: must not be negative" % where)
    return h


def need(d, key, where, typ=None):
    if key not in d:
        fail("%s: %r is required" % (where, key))
    v = d[key]
    if typ and not isinstance(v, typ):
        fail("%s: %r must be %s" % (where, key, typ.__name__ if isinstance(typ, type) else typ))
    return v


def check_keys(d, allowed, where):
    for k in d:
        if k not in allowed:
            fail("%s: unknown field %r" % (where, k))


def palm_text(v, where, maxlen=200):
    if not isinstance(v, str):
        fail("%s: must be text" % where)
    v = fold(v)
    if not is_palm(v):
        bad = [ch for ch in v if not is_palm(ch)]
        fail("%s: %r can't be drawn by the Palm font (only U+0020-U+00FF here)" % (where, bad[0]))
    if len(v.encode()) > maxlen:
        fail("%s: longer than %d bytes" % (where, maxlen))
    return v


class Course:
    """The parsed source: everything the file will hold, before it's encoded."""


def load_source(folder):
    folder = Path(folder)
    cj = folder / "course.json"
    try:
        cfg = json.loads(cj.read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail("%s: no course.json" % folder)
    except json.JSONDecodeError as e:
        fail("%s: %s" % (cj, e))
    W = "course.json"
    check_keys(cfg, {"format", "id", "title", "version", "author", "licence", "language",
                     "description", "source", "kinds", "roles", "scheduler", "grading",
                     "lessons", "unlock", "render", "levels"}, W)
    if cfg.get("format") != 1:
        fail("%s: \"format\" must be 1" % W)
    c = Course()
    c.folder = folder
    c.meta_text = []
    cid = need(cfg, "id", W, str)
    if not (1 <= len(cid) <= 31) or any(ch not in "abcdefghijklmnopqrstuvwxyz0123456789._-" for ch in cid):
        fail("%s: \"id\" must be 1-31 of a-z 0-9 . _ -" % W)
    c.meta_text.append((K_ID, cid))
    c.meta_text.append((K_TITLE, palm_text(need(cfg, "title", W), W + " title", 63)))
    for key, k in (("version", K_VERSION), ("author", K_AUTHOR), ("licence", K_LICENCE),
                   ("language", K_LANGUAGE), ("description", K_DESC), ("source", K_SOURCE)):
        if key in cfg:
            c.meta_text.append((k, palm_text(cfg[key], W + " " + key, 400)))

    tsv = (folder / "cards.tsv").exists()
    kinds = cfg.get("kinds")
    if kinds is None and tsv:
        kinds = [{"name": "card", "quiz": ["meaning"]}]
    if not isinstance(kinds, list) or not 1 <= len(kinds) <= MAX_KINDS:
        fail("%s: \"kinds\" must list 1-%d kinds" % (W, MAX_KINDS))
    c.kinds = []
    for i, k in enumerate(kinds):
        where = "%s kinds[%d]" % (W, i)
        check_keys(k, {"name", "quiz", "level_up"}, where)
        name = palm_text(need(k, "name", where), where + " name", 23)
        quiz = need(k, "quiz", where, list)
        mask = 0
        for q in quiz:
            if q not in ("meaning", "reading"):
                fail("%s: quiz is \"meaning\" and/or \"reading\"" % where)
            mask |= 1 if q == "meaning" else 2
        if not mask:
            fail("%s: quiz can't be empty" % where)
        if any(name == n for n, _, _ in c.kinds):
            fail("%s: two kinds named %r" % (where, name))
        c.kinds.append((name, mask, 1 if k.get("level_up") else 0))
    c.kind_index = {n: i for i, (n, _, _) in enumerate(c.kinds)}

    roles = cfg.get("roles", [])
    if not isinstance(roles, list) or len(roles) > MAX_ROLES:
        fail("%s: \"roles\" is a list of up to %d names" % (W, MAX_ROLES))
    c.roles = [palm_text(r, "%s roles[%d]" % (W, i), 15) for i, r in enumerate(roles)]

    s = need(cfg, "scheduler", W, dict)
    Ws = W + " scheduler"
    st = need(s, "type", Ws, str)
    c.meta_nums = []
    c.groups = []
    if st == "stages":
        check_keys(s, {"type", "intervals", "groups", "drop", "drop_high", "high_from", "known"}, Ws)
        iv = need(s, "intervals", Ws, list)
        if not 2 <= len(iv) <= 16 or iv[-1] is not None or None in iv[:-1]:
            fail("%s: \"intervals\" lists 2-16 stages, the last null (retired)" % Ws)
        secs = [duration(v, Ws + " intervals") for v in iv[:-1]] + [0]
        c.meta_nums.append((K_SCHED, [1]))
        c.meta_nums.append((K_STAGES, secs))
        drops = [s.get("drop", 1), s.get("drop_high", 2), s.get("high_from", 5)]
        if any(not isinstance(x, int) or x < 0 for x in drops) or not 1 <= drops[2] <= len(secs):
            fail("%s: bad drop, drop_high or high_from" % Ws)
        c.meta_nums.append((K_DROPS, drops))
        known = s.get("known", 5)
        if not isinstance(known, int) or not 1 <= known <= len(secs):
            fail("%s: \"known\" must be a stage, 1-%d" % (Ws, len(secs)))
        for i, g in enumerate(s.get("groups", [])):
            where = "%s groups[%d]" % (Ws, i)
            check_keys(g, {"from", "name"}, where)
            fr = need(g, "from", where)
            if not isinstance(fr, int) or not 1 <= fr <= len(secs):
                fail("%s: \"from\" is a stage number" % where)
            c.groups.append((fr, palm_text(need(g, "name", where), where + " name", 15)))
        default_grading = 2
    elif st == "sm2":
        check_keys(s, {"type", "ladder", "multiplier", "ease", "ease_change", "relearn",
                       "day_aligned", "known", "groups"}, Ws)
        lad = need(s, "ladder", Ws, list)
        if not 1 <= len(lad) <= 16:
            fail("%s: \"ladder\" has 1-16 rungs" % Ws)
        c.meta_nums.append((K_SCHED, [2]))
        c.meta_nums.append((K_LADDER, [duration(v, Ws + " ladder") for v in lad]))
        m = need(s, "multiplier", Ws, dict)
        check_keys(m, {"hard", "good", "easy"}, Ws + " multiplier")
        c.meta_nums.append((K_MULT, [hundredths(need(m, g, Ws + " multiplier"), Ws + " multiplier " + g)
                                     for g in ("hard", "good", "easy")]))
        e = need(s, "ease", Ws, dict)
        check_keys(e, {"start", "min"}, Ws + " ease")
        es = [hundredths(need(e, g, Ws + " ease"), Ws + " ease " + g) for g in ("start", "min")]
        if not 130 <= es[1] <= es[0] <= 385:
            fail("%s: ease must be 1.30-3.85 with min <= start" % Ws)
        c.meta_nums.append((K_EASE, es))
        ec = s.get("ease_change", {"again": -0.2, "hard": -0.15, "good": 0, "easy": 0.15})
        check_keys(ec, {"again", "hard", "good", "easy"}, Ws + " ease_change")
        c.meta_nums.append((K_EASE_CHANGE, [hundredths(ec.get(g, 0), Ws + " ease_change " + g, True)
                                            for g in ("again", "hard", "good", "easy")]))
        c.meta_nums.append((K_RELEARN, [duration(s.get("relearn", "10m"), Ws + " relearn")]))
        c.meta_nums.append((K_DAY_ALIGNED, [1 if s.get("day_aligned") else 0]))
        known = s.get("known", 1)
        if not isinstance(known, int) or not 1 <= known <= len(lad):
            fail("%s: \"known\" must be a rung, 1-%d" % (Ws, len(lad)))
        for i, g in enumerate(s.get("groups", [])):
            where = "%s groups[%d]" % (Ws, i)
            check_keys(g, {"from", "name"}, where)
            c.groups.append((duration(need(g, "from", where), where) // 3600,
                             palm_text(need(g, "name", where), where + " name", 15)))
        default_grading = 4
    else:
        fail("%s: \"type\" is \"stages\" or \"sm2\"" % Ws)
    if len(c.groups) > MAX_GROUPS:
        fail("%s: up to %d groups" % (Ws, MAX_GROUPS))
    if [g[0] for g in c.groups] != sorted(set(g[0] for g in c.groups)):
        fail("%s: groups must be in increasing order of \"from\"" % Ws)
    grading = cfg.get("grading")
    if grading not in (None, "two", "four"):
        fail("%s: \"grading\" is \"two\" or \"four\"" % W)
    c.meta_nums.append((K_GRADING, [default_grading if grading is None else (2 if grading == "two" else 4)]))
    les = cfg.get("lessons", {})
    check_keys(les, {"batch"}, W + " lessons")
    batch = les.get("batch", 5)
    if not isinstance(batch, int) or not 1 <= batch <= 20:
        fail("%s: lessons.batch is 1-20" % W)
    c.meta_nums.append((K_BATCH, [batch]))
    un = cfg.get("unlock", {})
    check_keys(un, {"level_percent", "by_links"}, W + " unlock")
    pct = un.get("level_percent", 90)
    if not isinstance(pct, int) or not 0 <= pct <= 100:
        fail("%s: unlock.level_percent is 0-100" % W)
    c.meta_nums.append((K_LEVEL_PCT, [pct]))
    c.meta_nums.append((K_BY_LINKS, [0 if un.get("by_links", True) is False else 1]))
    c.meta_nums.append((K_KNOWN, [known]))
    c.meta_nums.append((K_RENDER, [WRAP_W, H_PROMPT, H_WORD, H_TEXT]))
    rnd = cfg.get("render", {})
    check_keys(rnd, {"strip"}, W + " render")
    c.strip = set()
    for f in rnd.get("strip", []):
        if f not in STRIPPABLE:
            fail("%s: render.strip takes %s" % (W, ", ".join(sorted(STRIPPABLE))))
        c.strip.add(STRIPPABLE[f])

    # levels
    lv = cfg.get("levels")
    if lv is None:
        lv = [{"level": 1}]
    if not isinstance(lv, list) or not 1 <= len(lv) <= MAX_LEVELS:
        fail("%s: \"levels\" lists 1-%d levels" % (W, MAX_LEVELS))
    c.levels = []
    last = 0
    for i, l in enumerate(lv):
        where = "%s levels[%d]" % (W, i)
        check_keys(l, {"level", "title", "theme", "gloss", "part", "parts"}, where)
        n = need(l, "level", where)
        if not isinstance(n, int) or not last < n <= 65535:
            fail("%s: levels must be numbered 1-65535, increasing" % where)
        last = n
        part, parts = l.get("part", 1), l.get("parts", 1)
        if not (isinstance(part, int) and isinstance(parts, int) and 1 <= part <= parts <= 255):
            fail("%s: part and parts are 1-255, part <= parts" % where)
        fields = []
        for key, tag in (("title", LEVEL_TITLE), ("theme", LEVEL_THEME), ("gloss", LEVEL_GLOSS)):
            v = l.get(key)
            if v:
                if not isinstance(v, str):
                    fail("%s: %r must be text" % (where, key))
                fields.append([tag, 0, fold(v)])
        c.levels.append({"n": n, "part": part, "parts": parts, "fields": fields, "where": where})
    c.level_numbers = {l["n"] for l in c.levels}

    # items
    if tsv and (folder / "items.jsonl").exists():
        fail("%s: has both cards.tsv and items.jsonl" % folder)
    if tsv:
        raw = load_tsv(folder / "cards.tsv", c)
    else:
        raw = load_jsonl(folder / "items.jsonl")
    if not raw:
        fail("%s: no items" % folder)
    if len(raw) > MAX_ITEMS:
        fail("%s: %d items, over %d" % (folder, len(raw), MAX_ITEMS))
    c.items = [parse_item(r, where, c) for r, where in raw]
    seen = {}
    for it in c.items:
        if it["sid"] in seen:
            fail("%s: id %r is also used at %s" % (it["where"], it["sid"], seen[it["sid"]]))
        seen[it["sid"]] = it["where"]
    # sort by level, keeping source order within a level
    c.items.sort(key=lambda it: it["level"])
    c.index = {it["sid"]: i for i, it in enumerate(c.items)}
    for lvl in c.levels:
        if not any(it["level"] == lvl["n"] for it in c.items):
            fail("%s: level %d has no items" % (lvl["where"], lvl["n"]))
    resolve_links(c)
    return c


def load_jsonl(path):
    out = []
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        fail("%s: no items.jsonl (or cards.tsv)" % path.parent)
    for ln, line in enumerate(text.split("\n"), 1):
        if not line.strip():
            continue
        where = "%s:%d" % (path.name, ln)
        try:
            d = json.loads(line)
        except json.JSONDecodeError as e:
            fail("%s: %s" % (where, e))
        if not isinstance(d, dict):
            fail("%s: each line is one JSON object" % where)
        out.append((d, where))
    return out


def load_tsv(path, c):
    out = []
    kind = c.kinds[0][0]
    level = c.levels[0]["n"]
    for ln, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
        if not line.strip() or line.startswith("#"):
            continue
        cols = line.rstrip("\r").split("\t")
        where = "%s:%d" % (path.name, ln)
        if len(cols) not in (2, 3) or not cols[0] or not cols[1]:
            fail("%s: a card is front<TAB>back, or front<TAB>back<TAB>id" % where)
        out.append(({"id": cols[2] if len(cols) == 3 else cols[0], "kind": kind, "level": level,
                     "term": cols[0], "meanings": [cols[1]]}, where))
    return out


ITEM_KEYS = {"id", "kind", "level", "term", "meanings", "readings", "senses", "examples",
             "mnemonic_meaning", "mnemonic_reading", "etymology", "notes", "forms",
             "built_from", "unlock_after", "family", "related", "rank"}


def text_of(v, where):
    if not isinstance(v, str) or not v.strip():
        fail("%s: must be non-empty text" % where)
    v = fold(v.strip())
    if any(ord(ch) < 0x20 for ch in v):
        fail("%s: control characters aren't allowed" % where)
    return v


def parse_item(d, where, c):
    check_keys(d, ITEM_KEYS, where)
    sid = need(d, "id", where)
    if not isinstance(sid, str) or not sid or "\t" in sid or "\n" in sid:
        fail("%s: \"id\" must be non-empty text without tabs" % where)
    kind = need(d, "kind", where, str)
    if kind not in c.kind_index:
        fail("%s: kind %r isn't in course.json" % (where, kind))
    level = need(d, "level", where)
    if level not in c.level_numbers:
        fail("%s: level %r isn't in course.json" % (where, level))
    f = []  # [tag, attr-bits-0..6, text]
    f.append([TERM, 0, text_of(need(d, "term", where), where + " term")])
    ms = d.get("meanings", [])
    if not isinstance(ms, list):
        fail("%s: \"meanings\" is a list" % where)
    for i, m in enumerate(ms):
        f.append([MEANING, A_PRIMARY if i == 0 else 0, text_of(m, "%s meanings[%d]" % (where, i))])
    rs = d.get("readings", [])
    prim = 0
    for i, r in enumerate(rs):
        w = "%s readings[%d]" % (where, i)
        if isinstance(r, str):
            r = {"text": r}
        check_keys(r, {"text", "type", "primary"}, w)
        t = r.get("type", "word")
        if t not in READING_TYPES:
            fail("%s: type is on, kun, nanori or word" % w)
        p = bool(r.get("primary"))
        prim += p
        f.append([READING, (A_PRIMARY if p else 0) | READING_TYPES[t], text_of(need(r, "text", w), w)])
    if rs and prim == 0 and len(rs) == 1:
        f[-1][1] |= A_PRIMARY
    elif rs and prim != 1:
        fail("%s: exactly one reading must be \"primary\"" % where)
    kmask = c.kinds[c.kind_index[kind]][1]
    if kmask & 1 and not ms:
        fail("%s: kind %r is quizzed on meaning, so it needs \"meanings\"" % (where, kind))
    if kmask & 2 and not rs:
        fail("%s: kind %r is quizzed on reading, so it needs \"readings\"" % (where, kind))
    for i, s in enumerate(d.get("senses", [])):
        w = "%s senses[%d]" % (where, i)
        check_keys(s, {"pos", "gloss", "tags"}, w)
        pos = text_of(need(s, "pos", w), w + " pos")
        gl = text_of(need(s, "gloss", w), w + " gloss")
        tags = s.get("tags", [])
        v = pos + SEP + gl + (SEP + ",".join(text_of(t, w + " tags") for t in tags) if tags else "")
        f.append([SENSE, min(i + 1, 31) if i + 1 <= 31 else 0, v])
    nsenses = len(d.get("senses", []))
    for i, e in enumerate(d.get("examples", [])):
        w = "%s examples[%d]" % (where, i)
        check_keys(e, {"text", "translation", "sense"}, w)
        sn = e.get("sense", 0)
        if not isinstance(sn, int) or not 0 <= sn <= max(nsenses, 0) or sn > 31:
            fail("%s: \"sense\" must be one of the item's senses (1-%d)" % (w, nsenses))
        f.append([EXAMPLE, sn, text_of(need(e, "text", w), w + " text")])
        if "translation" in e:
            f.append([TRANSLATION, 0, text_of(e["translation"], w + " translation")])
    for key, tag in (("mnemonic_meaning", MNEM_M), ("mnemonic_reading", MNEM_R), ("etymology", ETYMOLOGY)):
        if key in d:
            f.append([tag, 0, text_of(d[key], where + " " + key)])
    for i, n in enumerate(d.get("notes", [])):
        f.append([NOTE, 0, text_of(n, "%s notes[%d]" % (where, i))])
    for i, fm in enumerate(d.get("forms", [])):
        w = "%s forms[%d]" % (where, i)
        check_keys(fm, {"tag", "form"}, w)
        f.append([FORM, 0, text_of(need(fm, "tag", w), w) + SEP + text_of(need(fm, "form", w), w)])
    rank = d.get("rank", 0)
    if not isinstance(rank, int) or rank < 0:
        fail("%s: \"rank\" is a whole number, 1 = most common" % where)
    links = []
    for key, typ in (("unlock_after", LINK_UNLOCK), ("built_from", LINK_BUILT), ("related", LINK_RELATED)):
        v = d.get(key, [])
        if not isinstance(v, list):
            fail("%s: %r is a list of ids" % (where, key))
        for x in v:
            role = 0
            if isinstance(x, dict):
                check_keys(x, {"id", "role"}, where + " " + key)
                if typ != LINK_BUILT and "role" in x:
                    fail("%s: only built_from links have a role" % where)
                r = x.get("role")
                if r is not None:
                    if r not in c.roles:
                        fail("%s: role %r isn't in course.json roles" % (where, r))
                    role = c.roles.index(r) + 1
                x = need(x, "id", where + " " + key)
            links.append((typ, x, role))
    if "family" in d:
        links.append((LINK_FAMILY, d["family"], 0))
    if len(links) > MAX_LINKS:
        fail("%s: %d links, over %d" % (where, len(links), MAX_LINKS))
    return {"sid": sid, "kind": c.kind_index[kind], "level": level, "fields": f, "rank": min(rank, 65535),
            "links": links, "where": where}


def resolve_links(c):
    for it in c.items:
        res = []
        for typ, target, role in it["links"]:
            if target not in c.index:
                fail("%s: %s links to %r, which isn't an item in this course"
                     % (it["where"], LINK_NAMES[typ], target))
            n = c.index[target]
            if c.items[n] is it:
                fail("%s: an item can't link to itself" % it["where"])
            res.append((n, typ, role))
        it["links"] = res
    # built_from + unlock_after must not loop
    state = [0] * len(c.items)
    for start in range(len(c.items)):
        if state[start]:
            continue
        stack = [(start, iter([n for n, t, _ in c.items[start]["links"] if t in (LINK_UNLOCK, LINK_BUILT)]))]
        state[start] = 1
        while stack:
            node, it = stack[-1]
            nxt = next(it, None)
            if nxt is None:
                state[node] = 2
                stack.pop()
            elif state[nxt] == 1:
                fail("%s: built_from/unlock_after links go round in a loop" % c.items[nxt]["where"])
            elif state[nxt] == 0:
                state[nxt] = 1
                stack.append((nxt, iter([n for n, t, _ in c.items[nxt]["links"] if t in (LINK_UNLOCK, LINK_BUILT)])))


# ---------------------------------------------------------------- ids.tsv

def assign_ids(c, write):
    path = c.folder / "ids.tsv"
    have, order, top = {}, [], 0
    if path.exists():
        for ln, line in enumerate(path.read_text(encoding="utf-8").split("\n"), 1):
            line = line.rstrip("\r")               # a Windows checkout
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 2 or not parts[1].isdigit():
                fail("ids.tsv:%d: a line is id<TAB>number" % ln)
            n = int(parts[1])
            if parts[0] in have or not 1 <= n <= 0xFFFFFFFF or n in have.values():
                fail("ids.tsv:%d: duplicate id or number" % ln)
            have[parts[0]] = n
            order.append(parts[0])
            top = max(top, n)
    new = []
    for it in c.items:
        if it["sid"] not in have:
            top += 1
            if top > 0xFFFFFFFF:
                fail("ids.tsv: out of numbers")
            have[it["sid"]] = top
            new.append(it["sid"])
        it["id"] = have[it["sid"]]
    if new and write:
        lines = ["# source id\tnumber -- kept by tools/mkcourse.py: numbers are never reused or "
                 "changed, so commit this file with the course"]
        lines += ["%s\t%d" % (k, have[k]) for k in order + new]
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return len(new)


# ---------------------------------------------------------------- encoding

def classify(c, painter_box, font_arg):
    """Decide how each field is drawn (COURSE_FORMAT.md section 4); render pictures."""
    def painter():
        if painter_box[0] is None:
            painter_box[0] = Painter(font_path(font_arg))
        return painter_box[0]

    def one(tag, attr, text, where):
        allow_sep = tag in (SENSE, FORM)
        if is_palm(text, allow_sep):
            return attr, text, None
        if is_kana(text):
            return attr | A_KANA, text, None
        if tag not in (TERM, READING):
            text = latin_fallback(text)
            if is_palm(text, allow_sep):
                return attr, text, None
        if tag in c.strip:
            s = strip_unpalm(text)
            if not any(ch.isalnum() for ch in s):
                return None                 # nothing readable left: drop it
            if is_palm(s):
                return attr, s, None
        return attr | A_PICTURE, text, painter().picture(tag, text, where)

    def encode_fields(fields, where):
        out, drop_translation = [], False
        for t, a, s in fields:
            if t == TRANSLATION and drop_translation:
                drop_translation = False
                c.dropped += 1
                continue
            drop_translation = False
            e = one(t, a, s, "%s %s" % (where, TAG_NAMES[t]))
            if e is None:
                c.dropped += 1
                drop_translation = t == EXAMPLE     # its translation goes with it
                continue
            out.append(e + (t,))
        return out

    c.dropped = 0
    for lvl in c.levels:
        lvl["enc"] = encode_fields(lvl["fields"], lvl["where"])
    for it in c.items:
        it["enc"] = encode_fields(it["fields"], it["where"])


def encode(c):
    """-> bytes of the whole file."""
    bmp = bytearray()
    bmp_at = {}

    def add_bmp(pic):
        w, h, stride, rows = pic
        key = (w, h, rows)
        if key not in bmp_at:
            bmp_at[key] = len(bmp)
            bmp.extend(struct.pack("<HHBBHI", w, h, 1, 0, stride, crc32(rows)) + rows)
            while len(bmp) % 4:
                bmp.append(0)
        return bmp_at[key]

    def blob(enc, where):
        b = bytearray()
        for attr, text, pic, tag in enc:
            t = text.encode("utf-8")
            if len(t) > 65535:
                fail("%s: a field is over 64 KB" % where)
            b += struct.pack("<BBH", tag, attr, len(t))
            if attr & A_PICTURE:
                b += struct.pack("<I", add_bmp(pic))
            b += t
        return bytes(b)

    text = bytearray()
    levl = bytearray()
    first = 0
    for lvl in c.levels:
        b = blob(lvl["enc"], lvl["where"])
        if len(b) > MAX_TEXT:
            fail("%s: its text is %d B, over %d" % (lvl["where"], len(b), MAX_TEXT))
        count = sum(1 for it in c.items if it["level"] == lvl["n"])
        levl += struct.pack("<HBBIIIHHI", lvl["n"], lvl["part"], lvl["parts"], first, count,
                            len(text), len(b), 0, crc32(b))
        text += b
        first += count
    item = bytearray()
    link = bytearray()
    for it in c.items:
        b = blob(it["enc"], it["where"])
        if len(b) > MAX_TEXT:
            fail("%s: its text is %d B, over the %d B limit" % (it["where"], len(b), MAX_TEXT))
        lk = b"".join(struct.pack("<IBBH", n, typ, role, 0) for n, typ, role in it["links"])
        loff = len(link) if it["links"] else 0
        item += struct.pack("<IHBBIHHIHHII", it["id"], it["level"], it["kind"], 0, len(text), len(b),
                            len(it["links"]), loff, it["rank"], 0, crc32(b + lk), 0)
        text += b
        link += lk
    idx = b"".join(struct.pack("<II", it["id"], n)
                   for n, it in sorted(enumerate(c.items), key=lambda p: p[1]["id"]))

    meta = bytearray()

    def mkey(k, v):
        nonlocal meta
        meta += struct.pack("<HH", k, len(v)) + v
    for k, s in c.meta_text[:2]:
        mkey(k, s.encode("utf-8"))
    for k, s in c.meta_text[2:]:
        mkey(k, s.encode("utf-8"))
    for name, mask, flags in c.kinds:
        mkey(K_KIND, struct.pack("<BB", mask, flags) + name.encode("utf-8"))
    for r in c.roles:
        mkey(K_ROLE, r.encode("utf-8"))
    for fr, name in c.groups:
        mkey(K_GROUP, struct.pack("<I", fr) + name.encode("utf-8"))
    for k, nums in c.meta_nums:
        mkey(k, struct.pack("<%d%s" % (len(nums), "i" if k in SIGNED_KEYS else "I"), *nums))
    if len(meta) > MAX_META:
        fail("META is %d B, over %d" % (len(meta), MAX_META))

    bodies = {b"META": bytes(meta), b"LEVL": bytes(levl), b"ITEM": bytes(item), b"TEXT": bytes(text),
              b"BMP ": bytes(bmp), b"LINK": bytes(link), b"IDX ": idx}
    off = 32 + 16 * len(SECTIONS)
    table = bytearray()
    body = bytearray()
    for tag in SECTIONS:
        while (off + len(body)) % 4:
            body.append(0)
        data = bodies[tag]
        table += tag + struct.pack("<III", off + len(body), len(data), crc32(data))
        body += data
    size = 32 + len(table) + len(body)
    if size >= 1 << 31:
        fail("the course is over 2 GB")
    head = MAGIC + struct.pack("<HHIHHIII", FORMAT_MAJOR, FORMAT_MINOR, size, len(SECTIONS), 0,
                               len(c.items), len(c.levels), crc32(table))
    head += struct.pack("<I", crc32(head))
    return bytes(head + table + body)


def build(folder, out=None, font=None, write_ids=True):
    c = load_source(folder)
    added = assign_ids(c, write_ids)
    classify(c, [None], font)
    data = encode(c)
    return c, data, added


# ---------------------------------------------------------------- reading back

class Reader:
    """An independent check of a built file, the same rules as course.c."""

    def __init__(self, data):
        self.d = data
        d = data
        if len(d) < 32 or d[:4] != MAGIC:
            fail("not a course file (no SRSC magic)")
        major, minor, size, nsec, _, self.n_items, self.n_levels, self.fp = struct.unpack_from("<HHIHHIII", d, 4)
        if major != FORMAT_MAJOR:
            fail("format version %d; this reads %d" % (major, FORMAT_MAJOR))
        if crc32(d[:28]) != struct.unpack_from("<I", d, 28)[0]:
            fail("the header's CRC is wrong")
        if size != len(d):
            fail("the file is %d B but says %d: truncated or padded" % (len(d), size))
        if not 5 <= nsec <= 16 or 32 + 16 * nsec > size:
            fail("bad section count %d" % nsec)
        table = d[32:32 + 16 * nsec]
        if crc32(table) != self.fp:
            fail("the section table's CRC is wrong")
        self.sec = {}
        end = 32 + 16 * nsec
        for i in range(nsec):
            tag = table[16 * i:16 * i + 4]
            off, ln, crc = struct.unpack_from("<III", table, 16 * i + 4)
            if off % 4 or off < end or off + ln > size:
                fail("section %r is out of place" % tag)
            if any(d[end:off]):
                fail("non-zero padding before %r" % tag)
            end = off + ln
            if tag in self.sec:
                fail("section %r twice" % tag)
            self.sec[tag] = (off, ln, crc)
        if end != size:
            fail("bytes after the last section")
        for t in REQUIRED:
            if t not in self.sec:
                fail("no %r section" % t)
        for t, (off, ln, crc) in self.sec.items():
            if crc32(d[off:off + ln]) != crc:
                fail("section %r: CRC is wrong" % t)

    def body(self, tag):
        if tag not in self.sec:
            return b""
        off, ln, _ = self.sec[tag]
        return self.d[off:off + ln]

    def meta(self):
        m = self.body(b"META")
        out, p = [], 0
        while p < len(m):
            if p + 4 > len(m):
                fail("META: a cut-off entry")
            k, n = struct.unpack_from("<HH", m, p)
            if p + 4 + n > len(m):
                fail("META: key %04x runs past the section" % k)
            out.append((k, m[p + 4:p + 4 + n]))
            p += 4 + n
        return out

    def fields(self, b, where):
        out, p = [], 0
        bmp = self.body(b"BMP ")
        while p < len(b):
            if p + 4 > len(b):
                fail("%s: a cut-off field" % where)
            tag, attr, n = struct.unpack_from("<BBH", b, p)
            p += 4
            pic = None
            if attr & A_PICTURE:
                if p + 4 > len(b):
                    fail("%s: a cut-off picture offset" % where)
                off = struct.unpack_from("<I", b, p)[0]
                p += 4
                if off % 4 or off + 12 > len(bmp):
                    fail("%s: picture offset out of range" % where)
                w, h, bpp, fl, stride, crc = struct.unpack_from("<HHBBHI", bmp, off)
                if not (1 <= w <= MAX_BMP_W and 1 <= h <= MAX_BMP_H and bpp in (1, 2)
                        and stride == (w * bpp + 7) // 8 and off + 12 + h * stride <= len(bmp)):
                    fail("%s: bad picture header" % where)
                rows = bmp[off + 12:off + 12 + h * stride]
                if crc32(rows) != crc:
                    fail("%s: picture CRC is wrong" % where)
                pic = (w, h, bpp, crc, off)
            if p + n > len(b):
                fail("%s: field text runs past the item" % where)
            out.append((tag, attr, pic, b[p:p + n].decode("utf-8")))
            p += n
        return out

    def levels(self):
        lv, text = self.body(b"LEVL"), self.body(b"TEXT")
        if len(lv) != 24 * self.n_levels:
            fail("LEVL is %d B for %d levels" % (len(lv), self.n_levels))
        out, nxt, last = [], 0, 0
        for i in range(self.n_levels):
            n, part, parts, first, count, toff, tlen, _, crc = struct.unpack_from("<HBBIIIHHI", lv, 24 * i)
            if n <= last or first != nxt or count < 1:
                fail("level %d: out of order, or not covering the items" % n)
            last, nxt = n, first + count
            if toff + tlen > len(text) or crc32(text[toff:toff + tlen]) != crc:
                fail("level %d: bad text" % n)
            out.append((n, part, parts, first, count, self.fields(text[toff:toff + tlen], "level %d" % n)))
        if nxt != self.n_items:
            fail("the levels cover %d items of %d" % (nxt, self.n_items))
        return out

    def items(self):
        it, text, link = self.body(b"ITEM"), self.body(b"TEXT"), self.body(b"LINK")
        if len(it) != 32 * self.n_items:
            fail("ITEM is %d B for %d items" % (len(it), self.n_items))
        out = []
        for i in range(self.n_items):
            iid, lvl, kind, fl, toff, tlen, nl, loff, rank, _, crc, _ = struct.unpack_from("<IHBBIHHIHHII", it, 32 * i)
            if not 1 <= tlen <= MAX_TEXT or toff + tlen > len(text) or nl > MAX_LINKS \
                    or (nl and (loff % 8 or loff + 8 * nl > len(link))):
                fail("item %d: bad offsets" % i)
            tb = text[toff:toff + tlen]
            lb = link[loff:loff + 8 * nl] if nl else b""
            if crc32(tb + lb) != crc:
                fail("item %d: CRC is wrong" % i)
            links = []
            for j in range(nl):
                tgt, typ, role, _ = struct.unpack_from("<IBBH", lb, 8 * j)
                if tgt >= self.n_items or tgt == i or typ not in LINK_NAMES:
                    fail("item %d: bad link" % i)
                links.append((tgt, typ, role))
            fields = self.fields(tb, "item %d" % i)
            if not fields or fields[0][0] != TERM or sum(f[0] == TERM for f in fields) != 1:
                fail("item %d: a TERM must come first, once" % i)
            out.append((iid, lvl, kind, rank, fields, links, tlen))
        ix = self.body(b"IDX ")
        if len(ix) != 8 * self.n_items:
            fail("IDX is %d B for %d items" % (len(ix), self.n_items))
        prev = 0
        for j in range(self.n_items):
            iid, n = struct.unpack_from("<II", ix, 8 * j)
            if iid <= prev or n >= self.n_items or out[n][0] != iid:
                fail("IDX entry %d is wrong" % j)
            prev = iid
        return out


# ---------------------------------------------------------------- the dump

def esc(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '"' or ch == "\\":
            out.append("\\" + ch)
        elif o < 0x20:
            out.append("\\x%02x" % o)
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def dump_meta_entry(k, v):
    if k in TEXT_KEYS:
        return "meta %04x %s" % (k, esc(v.decode("utf-8")))
    if k == K_KIND:
        return "meta %04x kind quiz=%d flags=%d %s" % (k, v[0], v[1], esc(v[2:].decode("utf-8")))
    if k == K_GROUP:
        return "meta %04x group from=%d %s" % (k, struct.unpack_from("<I", v)[0], esc(v[4:].decode("utf-8")))
    fmt = "<%d%s" % (len(v) // 4, "i" if k in SIGNED_KEYS else "I")
    return "meta %04x nums %s" % (k, " ".join(str(x) for x in struct.unpack(fmt, v)))


def dump_field(tag, attr, pic, text):
    s = "  field %02x attr=%02x" % (tag, attr)
    if pic:
        s += " pic=%dx%d/%d crc=%08x" % (pic[0], pic[1], pic[2], pic[3])
    return s + " " + esc(text)


def dump_file(data):
    r = Reader(data)
    lines = ["course items=%d levels=%d fingerprint=%08x" % (r.n_items, r.n_levels, r.fp)]
    lines += [dump_meta_entry(k, v) for k, v in r.meta()]
    items = r.items()
    for n, part, parts, first, count, fields in r.levels():
        lines.append("level %d part=%d/%d first=%d count=%d" % (n, part, parts, first, count))
        lines += [dump_field(*f) for f in fields]
        for i in range(first, first + count):
            iid, lvl, kind, rank, ifields, links, _ = items[i]
            lines.append("item %d id=%d level=%d kind=%d rank=%d" % (i, iid, lvl, kind, rank))
            lines += [dump_field(*f) for f in ifields]
            lines += ["  link %d -> %d role=%d" % (typ, tgt, role) for tgt, typ, role in links]
    return "\n".join(lines) + "\n"


def dump_source(c, data):
    """The same dump, from the model (before encoding) rather than from the bytes;
    only the fingerprint and picture checksums come from the encoded file."""
    fp = struct.unpack_from("<I", data, 24)[0]
    lines = ["course items=%d levels=%d fingerprint=%08x" % (len(c.items), len(c.levels), fp)]
    ents = [(k, s.encode("utf-8")) for k, s in c.meta_text]
    ents += [(K_KIND, struct.pack("<BB", m, f) + n.encode("utf-8")) for n, m, f in c.kinds]
    ents += [(K_ROLE, r.encode("utf-8")) for r in c.roles]
    ents += [(K_GROUP, struct.pack("<I", fr) + n.encode("utf-8")) for fr, n in c.groups]
    ents += [(k, struct.pack("<%d%s" % (len(v), "i" if k in SIGNED_KEYS else "I"), *v)) for k, v in c.meta_nums]
    lines += [dump_meta_entry(k, v) for k, v in ents]

    def pic(p):
        if not p:
            return None
        w, h, stride, rows = p
        return (w, h, 1, crc32(rows))
    first = 0
    for lvl in c.levels:
        count = sum(1 for it in c.items if it["level"] == lvl["n"])
        lines.append("level %d part=%d/%d first=%d count=%d" % (lvl["n"], lvl["part"], lvl["parts"], first, count))
        lines += [dump_field(t, a, pic(p), s) for a, s, p, t in lvl["enc"]]
        for i in range(first, first + count):
            it = c.items[i]
            lines.append("item %d id=%d level=%d kind=%d rank=%d" % (i, it["id"], it["level"], it["kind"], it["rank"]))
            lines += [dump_field(t, a, pic(p), s) for a, s, p, t in it["enc"]]
            lines += ["  link %d -> %d role=%d" % (typ, n, role) for n, typ, role in it["links"]]
        first += count
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------- --check

def check(path):
    try:
        data = Path(path).read_bytes()
    except OSError as e:
        fail("can't read %s: %s" % (path, e.strerror))
    r = Reader(data)
    meta = dict((k, v) for k, v in r.meta())
    for k in (K_ID, K_TITLE, K_SCHED):
        if k not in meta:
            fail("META has no key %04x" % k)
    kinds = [v[2:].decode() for k, v in r.meta() if k == K_KIND]
    levels = r.levels()
    items = r.items()
    for iid, lvl, kind, *_ in items:
        if kind >= len(kinds):
            fail("item %d: kind %d of %d" % (iid, kind, len(kinds)))
    print("%s: OK -- %s, %s" % (path, meta[K_ID].decode(), esc(meta[K_TITLE].decode())))
    print("  %d B; %d items in %d levels; fingerprint %08x" % (len(data), r.n_items, r.n_levels, r.fp))
    for t in SECTIONS:
        if t in r.sec:
            print("  %s %9d B" % (t.decode(), r.sec[t][1]))
    per_kind = [0] * len(kinds)
    for it in items:
        per_kind[it[2]] += 1
    print("  kinds: " + ", ".join("%s %d" % (k, n) for k, n in zip(kinds, per_kind)))
    pics, pic_tags, pic_bytes = set(), {}, {}
    for it in items:
        for tag, attr, pic, text in it[4]:
            if pic:
                pic_tags[tag] = pic_tags.get(tag, 0) + 1
                if pic[4] not in pics:
                    pics.add(pic[4])
                    pic_bytes[tag] = pic_bytes.get(tag, 0) + 12 + pic[1] * ((pic[0] * pic[2] + 7) // 8)
    print("  pictures: %d, %d B" % (len(pics), r.sec.get(b"BMP ", (0, 0, 0))[1]))
    for tag in sorted(pic_tags):
        print("    %-17s %4d fields, %7d B" % (TAG_NAMES.get(tag, "%02x" % tag), pic_tags[tag], pic_bytes.get(tag, 0)))
    big = sorted(items, key=lambda it: -it[6])[:3]
    print("  largest items: " + ", ".join("%s %d B" % (esc(it[4][0][3]), it[6]) for it in big))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="a course source folder (or a .srs file with --check/--dump)")
    ap.add_argument("-o", "--out", help="where to write the course (default: <folder>/course.srs)")
    ap.add_argument("--font", help="the pinned picture font, if you have it already")
    ap.add_argument("--check", action="store_true", help="validate a built .srs file and print its sizes")
    ap.add_argument("--dump", action="store_true", help="print the canonical dump of a folder or a .srs file")
    ap.add_argument("--no-ids", action="store_true", help="don't write ids.tsv (for a read-only check)")
    a = ap.parse_args()
    try:
        p = Path(a.path)
        if a.check:
            check(p)
        elif a.dump:
            if p.is_dir():
                c, data, _ = build(p, font=a.font, write_ids=False)
                sys.stdout.write(dump_source(c, data))
            else:
                sys.stdout.write(dump_file(p.read_bytes()))
        else:
            c, data, added = build(p, font=a.font, write_ids=not a.no_ids)
            out = Path(a.out) if a.out else p / "course.srs"
            tmp = out.with_suffix(out.suffix + ".tmp")
            tmp.write_bytes(data)
            tmp.replace(out)
            print("mkcourse: %s: %d items, %d levels, %d B%s%s" % (
                out, len(c.items), len(c.levels), len(data),
                ", %d new ids in ids.tsv" % added if added else "",
                ", %d stripped fields dropped (nothing readable left)" % c.dropped if c.dropped else ""))
    except CourseError as e:
        print("mkcourse: %s" % e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
