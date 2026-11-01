#!/usr/bin/env python3
"""Turn a drawing of a critter into its 128 x 88 battle sheet.

    python3 tools/mksheet.py tools/game_art/src/emberkit.jpg -o tools/game_art/emberkit-sheet.png
    python3 tools/mksheet.py SRC -o OUT --preview look.png   # also a 4x picture to judge it by
    python3 tools/mksheet.py --check SRC OUT                 # OUT is what SRC builds, to the byte

The drawing is pixel art blown up (a JPEG or PNG of any size, as an image
generator or a paint program exports it), on a white or checkerboard
background (light or dark), laid out like Tuxemon's sheets:

    +---------------------+---------------------+
    |                     |                     |
    |       front         |        back         |
    |                     |                     |
    +-------+-------+-----+---------------------+
    | icon  | icon  |
    +-------+-------+

The two icons are optional: with only a front and a back, the icons are the
front made small. Nothing has to be on an exact grid. The tool:

1. finds the background: its greys are learnt from the picture's border
   (white, a light checkerboard or a dark one), and it's those greys that reach
   the edge of the picture, and the holes in the drawing that show the checkerboard or
   are too big for an eye's white (so the white of an eye stays);
2. finds the drawings (front, back, two icons) as the groups of what's left,
   the two biggest being the front (left) and the back. A splash or a flame
   apart from the body joins the big piece it's nearest to, never both;
3. finds the drawing's pixel size from where the colour changes, across all
   of them together, then each drawing's own offset (generated art often
   sits a little differently in each);
4. takes each of the drawing's pixels as the median of the middle of its
   cell, clear if the cell is mostly background, and peels light grey
   fringe off the edges;
5. halves it (or more) until everything fits its place: a drawing made at
   twice the game's size is common. Each block becomes its own medoid
   colour, or its outline's colour if it has any, so lines and eyes hold;
6. places them in the sheet: the front centred with its feet 6 px from the
   bottom, the back centred with its feet 1 px from the bottom, the icons
   centred;
7. reduces the front and back together to 15 colours (the game's budget,
   GAME_FORMAT.md 3.9), keeping small, distinct colours (eyes, gems,
   highlights) over shades of big areas; the icons get their own 15.

The result is the same to the byte every time (Pillow is pinned in
tools/requirements-game.txt). Pure Pillow: no numpy.
"""
import argparse
import io
import sys
from collections import deque
from pathlib import Path

SHEET_W, SHEET_H = 128, 88
SPRITE = 64
ICON = 24
COLOURS = 15
FRONT_FOOT, BACK_FOOT = 6, 1            # free rows under the feet
BG_MIN, BG_SPREAD = 176, 28            # background: every channel >= BG_MIN, max - min <= BG_SPREAD


def fail(msg):
    print(f"mksheet: {msg}", file=sys.stderr)
    sys.exit(1)


def wdist(a, b):
    """Squared colour distance, weighted the way eyes weigh the channels."""
    dr, dg, db = a[0] - b[0], a[1] - b[1], a[2] - b[2]
    return 2 * dr * dr + 4 * dg * dg + 3 * db * db


# ---------------------------------------------------------------- 1, 2: background and drawings

class Backdrop:
    """What the background looks like, learnt from the picture's border. A light one
    (white, or the usual light checkerboard) is every light grey; a dark one (a
    dark checkerboard) is the greys between its two tones, with a margin for JPEG.
    `two_tones(c)` says which of the checkerboard's tones a pixel is nearer, for
    telling a hole that shows the checkerboard from an eye's white or its pupil;
    `fringe(c)` is a sampled cell that is the background blurred into the drawing."""

    def __init__(self, px, w, h):
        border = [px[y * w + x] for x in range(0, w, 2) for y in (0, h - 1)]
        border += [px[y * w + x] for y in range(0, h, 2) for x in (0, w - 1)]
        grey = sorted(min(c) for c in border if max(c) - min(c) <= BG_SPREAD)
        if len(grey) < len(border) // 2:
            fail("the picture's border isn't a grey background (white or a checkerboard)")
        mid = grey[len(grey) // 2]
        self.light = mid >= BG_MIN
        if self.light:
            self.lo, self.hi, self.split = BG_MIN, 255, 224
        else:
            # the checkerboard's two tones: the 10th and 90th centiles of the border's greys
            t0, t1 = grey[len(grey) // 10], grey[len(grey) * 9 // 10]
            self.lo, self.hi, self.split = max(0, t0 - 12), min(255, t1 + 16), (t0 + t1) // 2
        self.tones = "light" if self.light else f"dark, its greys {self.lo}..{self.hi}"

    def bgish(self, c):
        return max(c) - min(c) <= BG_SPREAD and min(c) >= self.lo and max(c) <= self.hi

    def checker(self, hole):
        """How many of a hole's pixels are each of the two tones (light first)."""
        if self.light:
            return (sum(1 for c in hole if min(c) >= 232), sum(1 for c in hole if min(c) < 216))
        return (sum(1 for c in hole if min(c) > self.split + 8), sum(1 for c in hole if max(c) < self.split - 8))

    def fringe(self, c):
        if self.light:
            return min(c) >= 150 and max(c) - min(c) <= 40
        return max(c) - min(c) <= 20 and min(c) >= self.lo and max(c) <= self.hi


def background(px, w, h, bd):
    """1 where the pixel is background: the backdrop's greys, connected to the picture's edge."""
    bgish = bd.bgish
    bg = bytearray(w * h)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            q.append(y * w + x)
    for y in range(h):
        for x in (0, w - 1):
            q.append(y * w + x)
    while q:
        i = q.popleft()
        if bg[i] or not bgish(px[i]):
            continue
        bg[i] = 1
        x, y = i % w, i // w
        if x > 0:
            q.append(i - 1)
        if x < w - 1:
            q.append(i + 1)
        if y > 0:
            q.append(i - w)
        if y < h - 1:
            q.append(i + w)
    # holes: background showing through the drawing (between an arm and the body).
    # A hole is a background-grey patch the edge doesn't reach that shows the checkerboard
    # (both its tones) or is too big to be an eye's white or a highlight.
    seen = bytearray(bg)
    big = w * h // 2000
    for start in range(w * h):
        if seen[start] or not bgish(px[start]):
            continue
        seen[start] = 1
        q, hole = [start], []
        while q:
            i = q.pop()
            hole.append(i)
            x, y = i % w, i // w
            for j, ok in ((i - 1, x > 0), (i + 1, x < w - 1), (i - w, y > 0), (i + w, y < h - 1)):
                if ok and not seen[j] and bgish(px[j]):
                    seen[j] = 1
                    q.append(j)
        light, grey = bd.checker([px[i] for i in hole])
        if len(hole) >= big or (len(hole) >= 64 and min(light, grey) >= len(hole) // 6):
            for i in hole:
                bg[i] = 1
    return bg


def components(bg, w, h, least, label=None):
    """Bounding boxes (x0, y0, x1, y1, pixels) of the 8-connected foreground pieces.
    With `label` (a list of w * h), each kept piece's pixels are set to its index."""
    seen = bytearray(bg)
    out = []
    for start in range(w * h):
        if seen[start]:
            continue
        seen[start] = 1
        q = [start]
        x0 = x1 = start % w
        y0 = y1 = start // w
        n = 0
        mine = []
        while q:
            i = q.pop()
            n += 1
            mine.append(i)
            x, y = i % w, i // w
            x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
            for dy in (-1, 0, 1):
                yy = y + dy
                if not 0 <= yy < h:
                    continue
                for dx in (-1, 0, 1):
                    xx = x + dx
                    if 0 <= xx < w:
                        j = yy * w + xx
                        if not seen[j]:
                            seen[j] = 1
                            q.append(j)
        if n >= least:
            if label is not None:
                for i in mine:
                    label[i] = len(out)
            out.append([x0, y0, x1 + 1, y1 + 1, n])
    return out


def nearest_big(pieces, label, w, h, gap):
    """The pieces near a big one (a quarter of the biggest or more) joined to it, each to
    the big piece whose pixels are nearest (not whose box is: a splash between the front's
    tornado and the back's tail belongs to the nearer). Returns the pieces left."""
    big = max(p[4] for p in pieces) // 4
    owner = [-1] * (w * h)
    dist = [0] * (w * h)
    q = deque()
    for i in range(w * h):
        k = label[i]
        if k >= 0 and pieces[k][4] >= big:
            owner[i] = k
            q.append(i)
    near = {}                          # small piece -> (distance, big piece)
    while q:
        i = q.popleft()
        d = dist[i]
        k = label[i]
        if k >= 0 and pieces[k][4] < big:
            if k not in near or (d, owner[i]) < near[k]:
                near[k] = (d, owner[i])
        if d >= gap:
            continue
        x, y = i % w, i // w
        for j, ok in ((i - 1, x > 0), (i + 1, x < w - 1), (i - w, y > 0), (i + w, y < h - 1)):
            if ok and owner[j] < 0:
                owner[j] = owner[i]
                dist[j] = d + 1
                q.append(j)
    out = [list(p[:5]) + [[k]] for k, p in enumerate(pieces)]
    for k, (_, b) in sorted(near.items()):
        a, p = out[b], pieces[k]
        out[b] = [min(a[0], p[0]), min(a[1], p[1]), max(a[2], p[2]), max(a[3], p[3]), a[4] + p[4], a[5] + [k]]
    return [p for k, p in enumerate(out) if k not in near]


def group(boxes, gap):
    """Join pieces whose boxes come within `gap` of each other (a flame apart from its ear),
    the closest pair first, but never two big ones (a quarter of the biggest or more):
    those are the front and the back, however close a tail comes to the other's tornado."""
    big = max(b[4] for b in boxes) // 4
    boxes = [list(b[:5]) + [b[4] >= big, list(b[5]) if len(b) > 5 else []] for b in boxes]

    def apart(a, b):
        dx = max(a[0] - b[2], b[0] - a[2], 0)
        dy = max(a[1] - b[3], b[1] - a[3], 0)
        cx = (a[0] + a[2] - b[0] - b[2]) ** 2 + (a[1] + a[3] - b[1] - b[3]) ** 2
        return max(dx, dy), cx

    while True:
        best = None
        for i in range(len(boxes)):
            for j in range(i + 1, len(boxes)):
                a, b = boxes[i], boxes[j]
                if a[5] and b[5]:
                    continue
                d = apart(a, b)
                if d[0] < gap and (best is None or d < best[0]):
                    best = (d, i, j)
        if best is None:
            return [b[:5] + [b[6]] for b in boxes]
        _, i, j = best
        a, b = boxes[i], boxes[j]
        boxes[i] = [min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]), a[4] + b[4], a[5] or b[5],
                    a[6] + b[6]]
        del boxes[j]


def edge_signal(px, bg, w, box, axis):
    """How much the colour changes across each column (axis 0) or row (axis 1) boundary."""
    x0, y0, x1, y1 = box[:4]

    def lum(i):
        return 765 if bg[i] else sum(px[i])
    if axis == 0:
        sig = [0] * (x1 - x0)
        for y in range(y0, y1):
            row = y * w
            prev = lum(row + x0)
            for x in range(x0 + 1, x1):
                cur = lum(row + x)
                sig[x - x0] += abs(cur - prev)
                prev = cur
    else:
        sig = [0] * (y1 - y0)
        for x in range(x0, x1):
            prev = lum(y0 * w + x)
            for y in range(y0 + 1, y1):
                cur = lum(y * w + x)
                sig[y - y0] += abs(cur - prev)
                prev = cur
    return sig


def comb(sig, p):
    """(score, phase) of the pixel size p: how much more of the change falls on one
    phase (within 0.75 px) than if it fell anywhere. A multiple of the true size
    gathers only some of the boundaries and a fraction of it gathers more phases,
    so both score lower than the size itself."""
    res, win = 0.25, 6                       # quarter-pixel bins, a 1.5 px window
    nb = max(win + 1, int(p / res + 0.5))
    acc = [0.0] * nb
    total = 0
    for i, v in enumerate(sig):
        if v:
            acc[int((i % p) / p * nb) % nb] += v
            total += v
    best, at = -1.0, 0
    run = sum(acc[k % nb] for k in range(win))
    for k in range(nb):
        if run > best:
            best, at = run, k
        run += acc[(k + win) % nb] - acc[k]
    return best / (total or 1) - win / nb, ((at + win / 2) / nb * p) % p


def art_pixel(sigs, lo, hi):
    """The drawing's pixel size, from every drawing's columns and rows together."""
    best = None
    p100 = int(lo * 50) * 2
    while p100 <= hi * 100:
        s = sum(comb(g, p100 / 100)[0] for g in sigs)
        if best is None or s > best[0]:
            best = (s, p100 / 100)
        p100 += 2
    return best[1]


def phase(sig, p):
    """The pixel size near p (within 1 %) and the offset that fit this one signal best."""
    return max((comb(sig, q / 1000) + (q / 1000,) for q in range(int(p * 990), int(p * 1010) + 1)),
               key=lambda t: (t[0], -abs(t[2] - p)))


def sample(px, bg, w, h, box, gx, gy, bd):
    """The drawing's own pixels: the median of each cell's middle (a list of rows of
    RGB tuples, None where the cell is mostly background)."""
    (pw, ox), (ph, oy) = gx, gy
    x0, y0, x1, y1 = box[:4]
    i0, i1 = int((x0 - ox) // pw), int((x1 - ox) // pw) + 1
    j0, j1 = int((y0 - oy) // ph), int((y1 - oy) // ph) + 1
    rows = []
    for j in range(j0, j1):
        ys = range(max(int(oy + (j + 0.3) * ph + 0.5), y0), min(int(oy + (j + 0.7) * ph + 0.5), y1))
        row = []
        for i in range(i0, i1):
            xs = range(max(int(ox + (i + 0.3) * pw + 0.5), x0), min(int(ox + (i + 0.7) * pw + 0.5), x1))
            fg, n = [], 0
            for y in ys:
                for x in xs:
                    n += 1
                    k = y * w + x
                    if not bg[k]:
                        fg.append(px[k])
            if n and 2 * len(fg) > n:
                row.append(tuple(sorted(v[ch] for v in fg)[len(fg) // 2] for ch in range(3)))
            else:
                row.append(None)
        rows.append(row)
    # the fringe: background-grey cells on the drawing's edge are the background's, blurred in
    changed = True
    while changed:
        changed = False
        for j, row in enumerate(rows):
            for i, c in enumerate(row):
                if c is None or not bd.fringe(c):
                    continue
                if any(not (0 <= j + dj < len(rows) and 0 <= i + di < len(row)) or rows[j + dj][i + di] is None
                       for dj, di in ((0, -1), (0, 1), (-1, 0), (1, 0))):
                    row[i] = None
                    changed = True
    return rows


def lum(c):
    return 2 * c[0] + 5 * c[1] + c[2]


def dark(c):
    """An outline's or a pupil's colour: a line one pixel wide in the drawing must
    survive being halved, so a block with any of it is drawn in it."""
    return lum(c) < 8 * 48 and max(c) < 96


def shrink(rows, k):
    """The drawing at 1/k: each k x k block becomes its medoid (the one of its own
    colours closest to the rest, the darker on a tie), its darkest ink if it has any
    (so outlines and eyes hold), or clear if most of it is. Of the k x k ways to lay the blocks, the one whose blocks are most
    alike. Returns an RGBA image cropped to what's drawn."""
    from PIL import Image
    h, w = len(rows), len(rows[0])
    best = None
    for oy in range(k):
        for ox in range(k):
            bw, bh = (w + ox + k - 1) // k, (h + oy + k - 1) // k
            out, spread = [], 0
            for by in range(bh):
                for bx in range(bw):
                    cells = [rows[y][x] for y in range(by * k - oy, by * k - oy + k)
                             for x in range(bx * k - ox, bx * k - ox + k) if 0 <= y < h and 0 <= x < w]
                    cs = [c for c in cells if c is not None]
                    if 2 * len(cs) < k * k:
                        out.append((0, 0, 0, 0))
                        continue
                    ink = [c for c in cs if dark(c)]
                    m = min(ink or cs, key=lambda c: (sum(wdist(c, d) for d in cs), lum(c), c))
                    spread += sum(wdist(m, d) for d in cs)
                    out.append(m + (255,))
            if best is None or spread < best[0]:
                best = (spread, bw, bh, out)
    _, bw, bh, out = best
    im = Image.new("RGBA", (bw, bh))
    im.putdata(out)
    bb = im.getchannel("A").getbbox()
    return im.crop(bb) if bb else im


# ---------------------------------------------------------------- 6: colours

def reduce_colours(images, colours=COLOURS):
    """One palette of at most `colours` for the images together, then the images in it.
    Median cut to 64 first, then the closest two are joined until `colours` are left,
    the distance scaled by the smaller one's size, so a few pixels of a distinct colour
    (an eye, a gem) outlast a shade of a big area."""
    from PIL import Image
    opaque = [p[:3] for im in images for p in im.get_flattened_data() if p[3] >= 128]
    if not opaque:
        fail("a drawing has no pixels")
    strip = Image.new("RGB", (len(opaque), 1))
    strip.putdata(opaque)
    q = strip.quantize(64, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = q.getpalette()
    groups = {}
    for c, k in zip(opaque, q.get_flattened_data()):
        g = groups.setdefault(k, [0, 0, 0, 0])
        g[0] += c[0]; g[1] += c[1]; g[2] += c[2]; g[3] += 1
    cl = sorted([[g[0] / g[3], g[1] / g[3], g[2] / g[3], g[3]] for g in groups.values()])
    del pal
    while len(cl) > colours:
        best = None
        for a in range(len(cl)):
            for b in range(a + 1, len(cl)):
                d = wdist(cl[a], cl[b]) * min(cl[a][3], cl[b][3]) ** 0.5
                if best is None or d < best[0]:
                    best = (d, a, b)
        _, a, b = best
        A, B = cl[a], cl[b]
        n = A[3] + B[3]
        cl[a] = [(A[k] * A[3] + B[k] * B[3]) / n for k in range(3)] + [n]
        del cl[b]
    palette = sorted({tuple(int(v + 0.5) for v in c[:3]) for c in cl})
    for _ in range(2):           # two rounds of moving each colour to the mean of its pixels
        sums = {c: [0, 0, 0, 0] for c in palette}
        for c in opaque:
            s = sums[min(palette, key=lambda p: (wdist(p, c), p))]
            s[0] += c[0]; s[1] += c[1]; s[2] += c[2]; s[3] += 1
        palette = sorted({tuple(int(v / s[3] + 0.5) for v in s[:3]) if s[3] else c
                          for c, s in sums.items()})
    memo = {}
    out = []
    for im in images:
        o = Image.new("RGBA", im.size, (0, 0, 0, 0))
        o.putdata([(memo.setdefault(p[:3], min(palette, key=lambda q: (wdist(q, p), q))) + (255,))
                   if p[3] >= 128 else (0, 0, 0, 0) for p in im.get_flattened_data()])
        out.append(o)
    return out, palette


# ---------------------------------------------------------------- the whole

def fit(im, w, h, what, notes):
    """`im` shrunk (nearest, whole steps of the art) if it doesn't fit w x h."""
    from PIL import Image
    if im.width <= w and im.height <= h:
        return im
    s = min(w / im.width, h / im.height)
    nw, nh = max(1, int(im.width * s)), max(1, int(im.height * s))
    notes.append(f"{what} is {im.width} x {im.height} art pixels, more than {w} x {h}: shrunk to {nw} x {nh}")
    return im.resize((nw, nh), Image.Resampling.NEAREST)


def build(src, notes, scale=None):
    from PIL import Image
    im = Image.open(src).convert("RGB")
    w, h = im.size
    px = list(im.get_flattened_data())
    bd = Backdrop(px, w, h)
    notes.append(f"the background: {bd.tones}")
    bg = background(px, w, h, bd)
    label = [-1] * (w * h)
    pieces = components(bg, w, h, least=max(16, w * h // 20000), label=label)
    gap = max(4, w // 64)
    parts = group(nearest_big(pieces, label, w, h, gap), gap)
    parts = [p for p in parts if p[4] >= max(q[4] for q in parts) // 50]
    if len(parts) not in (2, 4):
        fail(f"{src}: found {len(parts)} drawings, want a front, a back and two icons "
             f"(or just a front and a back): {[p[:4] for p in parts]}")
    parts.sort(key=lambda p: -p[4])
    boxes = sorted(parts[:2], key=lambda p: p[0]) + sorted(parts[2:], key=lambda p: p[0])
    names = ("the front", "the back", "icon 1", "icon 2")[:len(boxes)]
    # each drawing sees only its own pieces (and specks too small to be pieces): where
    # two boxes overlap, the other drawing's pixels are background to it
    masks = []
    for b in boxes:
        mine = set(b[5])
        m = bytearray(bg)
        for y in range(b[1], b[3]):
            for i in range(y * w + b[0], y * w + b[2]):
                if label[i] >= 0 and label[i] not in mine:
                    m[i] = 1
        masks.append(m)
    sigs = [(edge_signal(px, m, w, b, 0), edge_signal(px, m, w, b, 1)) for b, m in zip(boxes, masks)]
    hi = min(40.0, min(min(b[2] - b[0], b[3] - b[1]) for b in boxes) / 6)
    p = art_pixel([s for pair in sigs for s in pair], 3.0, max(4.0, hi))
    notes.append(f"the drawing's pixel: {p:.2f} of the picture's")
    arts = []
    for name, box, m, (sx, sy) in zip(names, boxes, masks, sigs):
        gx, gy = phase(sx, p), phase(sy, p)
        rows = sample(px, m, w, h, box, (gx[2], gx[1]), (gy[2], gy[1]), bd)
        arts.append(rows)
        notes.append(f"{name}: at ({box[0]},{box[1]})-({box[2]},{box[3]}), its pixel {gx[2]:.3f} x {gy[2]:.3f}, "
                     f"{len(rows[0])} x {len(rows)}")
    room = [(SPRITE, SPRITE - FRONT_FOOT), (SPRITE, SPRITE - BACK_FOOT), (ICON, ICON), (ICON, ICON)]
    if scale is None:
        scale = 1
        while any(-(-len(r[0]) // scale) > rw or -(-len(r) // scale) > rh for r, (rw, rh) in zip(arts, room)):
            scale += 1
        notes.append(f"scale: 1/{scale} (the smallest at which every drawing fits)")
    shrunk = [shrink(r, scale) for r in arts]
    front = fit(shrunk[0], *room[0], "the front", notes)
    back = fit(shrunk[1], *room[1], "the back", notes)
    if len(shrunk) == 4:
        ic = [fit(a, ICON, ICON, f"icon {k + 1}", notes) for k, a in enumerate(shrunk[2:])]
    else:
        notes.append("no icons drawn: both are the front made small")
        s = min(ICON / front.width, ICON / front.height, 1)
        small = front.resize((max(1, round(front.width * s)), max(1, round(front.height * s))),
                             Image.Resampling.NEAREST)
        ic = [small, small]
    for name, a in zip(names, [front, back] + ic):
        notes.append(f"{name}: {a.width} x {a.height} in the sheet")
    (front, back), pal = reduce_colours([front, back])
    notes.append(f"front and back: {len(pal)} colours")
    ic, ipal = reduce_colours(ic)
    notes.append(f"icons: {len(ipal)} colours")
    sheet = Image.new("RGBA", (SHEET_W, SHEET_H), (0, 0, 0, 0))
    sheet.paste(front, ((SPRITE - front.width) // 2, SPRITE - FRONT_FOOT - front.height))
    sheet.paste(back, (SPRITE + (SPRITE - back.width) // 2, SPRITE - BACK_FOOT - back.height))
    for k, a in enumerate(ic):
        sheet.paste(a, (k * ICON + (ICON - a.width) // 2, SPRITE + (ICON - a.height) // 2))
    return sheet


def png(im):
    b = io.BytesIO()
    im.save(b, "PNG", optimize=False, compress_level=9)
    return b.getvalue()


def preview(src, sheet, path):
    """The drawing beside the sheet at 4x on a grey ground, to judge the conversion by."""
    from PIL import Image
    z = 4
    s = sheet.resize((SHEET_W * z, SHEET_H * z), Image.Resampling.NEAREST)
    o = Image.open(src).convert("RGB")
    o = o.resize((round(o.width * s.height / o.height), s.height), Image.Resampling.LANCZOS)
    out = Image.new("RGB", (o.width + s.width + 8, s.height), (128, 128, 128))
    out.paste(o, (0, 0))
    out.paste(s, (o.width + 8, 0), s)
    out.save(path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("src", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--preview")
    ap.add_argument("--check", nargs=2, metavar=("SRC", "SHEET"))
    ap.add_argument("--scale", type=int, help="take every N x N of the drawing's pixels as one "
                    "(default: the smallest N at which everything fits)")
    a = ap.parse_args(argv)
    notes = []
    if a.check:
        want = png(build(a.check[0], notes, a.scale))
        if Path(a.check[1]).read_bytes() != want:
            fail(f"{a.check[1]} isn't what {a.check[0]} builds (run mksheet.py {a.check[0]} -o {a.check[1]})")
        print(f"{a.check[1]}: OK")
        return
    if not a.src or not a.out:
        ap.error("want SRC and -o OUT (or --check SRC SHEET)")
    sheet = build(a.src, notes, a.scale)
    Path(a.out).write_bytes(png(sheet))
    if a.preview:
        preview(a.src, sheet, a.preview)
    print("\n".join(notes))
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
