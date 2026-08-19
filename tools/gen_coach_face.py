#!/usr/bin/env python3
"""Emit the Coach portrait as an LVGL A8 image descriptor for palm_icons.c.

The weekly report ("Week" on Coach's home screen) stands the coach beside the
statistics and hangs his advice off him in a speech bubble. FACE below is the
supplied portrait -- beanie, headband, long hair, moustache -- reduced to the
60x65 that fits in the margin right of the 164 px stat column on a 240 px
screen. It was derived from the source artwork once, by binarising at 50%%
grey, cropping to the ink, and box-filtering down with a 50%% ink-coverage
threshold; `--from-image` below reruns exactly that recipe, so a new source or
a different target size is a one-liner rather than a redraw. The reduced art is
checked in as text because it is the thing that actually ships: it reviews as a
diff, and the normal path needs nothing but the standard library.

A8 (0 = transparent, 255 = ink) matches the launcher icons in palm_icons.c, so
ui.c recolors it to COL_LINE exactly the way it recolors those. It lives in
flash as const rodata -- 3900 bytes of a 3 MB app partition -- which is the
whole reason the portrait is an image and not a canvas: the 24 KB LVGL object
pool never sees it. See docs/BUILD_PROGRESS.md, "LVGL on a 24 KB object pool".

    python3 tools/gen_coach_face.py                  # the C, for palm_icons.c
    python3 tools/gen_coach_face.py --preview f.png [zoom]   # look at it (6x)
    python3 tools/gen_coach_face.py --from-exact f.png       # take edits back
    python3 tools/gen_coach_face.py --from-image src.png [W H]   # needs Pillow

`--preview f.png 1` and `--from-exact` are a lossless round trip: the art goes
out as a 60x65 (or integer-zoomed) black-on-white PNG, comes back pixel for
pixel, and prints the new FACE on stderr to paste above and the new C on stdout.
"""
import sys

FACE = """\
..........................#############.....................
......................#####################.................
......................######################................
....................##########################..............
..................##############################............
.................################################...........
................##################################..........
...............####################################.........
...............####################################.........
..............#######################################.......
..............#######################################.......
.............#########.............##################.......
.............######....................###############......
.............####.......................##############......
.............##............................###########......
............##.................................########.....
............#...................................#######.....
............#.....................................#####.....
...........##.....................................#####.....
...........#...........#########....................###.....
...........#......#####.........#####................##.....
...........#...####................####..............##.....
...........#..####.....................###............#.....
...........#######.......................###..........#.....
...........######.........................###.........#.....
...........###############..................###.......#.....
..........########.....####.......#######...####......#.....
.........########.................#########.#####.....#.....
........########...########......##.......########...#......
.......########...#..###..#.....########....#######..#......
#....##########...#..###.......##...#####...##########......
###############.....................###.##..#########.......
.##############..........................#...#########......
..############...............................#########......
...###########...............................#########......
....##########...............................#########......
......########...............................#########......
...###########...............................##########.....
....##########...............................###########....
.......#######..........#....................############...
.......#######..........###.....#...........################
.......#######.............######...........##############..
......########..........###.................##############..
......########......########.#####..........################
.....#########.....##################.......###############.
.....#########....###.#####...########.....#############....
.....##########...####.............####....#############....
....###########........###########...##....##############...
....###########............................##############...
....############..........................################..
...#############..........................################..
...#############.........................#################..
...##############.......................##################..
..################......................##################..
..################....................####################..
...#############.##..................###.#################..
...#############..##................##...################...
....###########....#...............##....################...
.....#########......#.............##.....###############....
......#######........###......####.......##############.....
........####..........##########........##############......
........................................############........
........................................##########..........
.......................................#########............
.......................................#######..............\
"""

GRID = [line for line in FACE.strip().split("\n")]
H = len(GRID)
W = len(GRID[0])
assert all(len(r) == W for r in GRID), "FACE rows are not all the same width"


def emit_c(grid=GRID, w=W, h=H):
    data = ",".join("255" if ch == "#" else "0" for row in grid for ch in row)
    print("/* Coach's portrait for the weekly report -- tools/gen_coach_face.py */")
    print("static const uint8_t coach_face_map[] = {%s};" % data)
    print("const lv_image_dsc_t coach_face = {")
    print("  .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.flags=0,"
          ".w=%d,.h=%d,.stride=%d,.reserved_2=0}," % (w, h, w))
    print("  .data_size=%d,.data=coach_face_map };" % (w * h))


def emit_png(path, grid=GRID, w=W, h=H, scale=6):
    """stdlib PNG, the same way sim/tools/ppm2png.py does it"""
    import struct, zlib
    rows = b""
    for y in range(h):
        line = b"\x00"
        for x in range(w):
            line += (b"\x00" if grid[y][x] == "#" else b"\xff") * scale
        rows += line * scale
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b""))
    print("wrote %s (%dx%d)" % (path, w * scale, h * scale), file=sys.stderr)


def from_image(path, w=None, h=None):
    """re-derive FACE from source artwork; aspect is taken from the ink bbox"""
    from PIL import Image                      # only this path needs Pillow
    im = Image.open(path).convert("L")
    ink = im.point(lambda v: 255 if v < 128 else 0)
    ink = ink.crop(ink.getbbox())              # trim the paper margin
    if w is None:
        w = 60
    if h is None:
        h = int(round(w * ink.size[1] / float(ink.size[0])))
    cov = ink.resize((w, h), Image.BOX).load() # per-target-pixel ink coverage
    grid = ["".join("#" if cov[x, y] >= 128 else "." for x in range(w))
            for y in range(h)]
    return grid, w, h


def read_png(path):
    """Minimal stdlib PNG reader -> (pixels, w, h) with pixels[y][x] = 1 for ink.

    Enough of the format to swallow whatever a pixel editor saves: 8/16-bit
    grey, RGB, RGBA and palette, with or without alpha. Ink is "dark and not
    transparent", so a white or empty background reads as paper either way.
    Interlaced files are refused rather than silently misread.
    """
    import struct, zlib
    raw = open(path, "rb").read()
    if raw[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("%s is not a PNG" % path)
    pos, idat, plte, trns = 8, b"", None, None
    while pos < len(raw):
        (ln,) = struct.unpack(">I", raw[pos:pos + 4])
        tag = raw[pos + 4:pos + 8]
        body = raw[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if   tag == b"IHDR": w, h, depth, ctype, _, _, ilace = struct.unpack(">IIBBBBB", body)
        elif tag == b"PLTE": plte = body
        elif tag == b"tRNS": trns = body
        elif tag == b"IDAT": idat += body
        elif tag == b"IEND": break
    if ilace:
        raise SystemExit("%s is interlaced; re-save it without Adam7" % path)
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, nch * depth // 8)                 # filter distance, in bytes
    stride = (w * nch * depth + 7) // 8
    data = zlib.decompress(idat)
    out, prev = [], bytearray(stride)
    for y in range(h):                             # undo the per-row filters
        f = data[y * (stride + 1)]
        row = bytearray(data[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = row[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if   f == 1: row[i] = (row[i] + a) & 255
            elif f == 2: row[i] = (row[i] + b) & 255
            elif f == 3: row[i] = (row[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out.append(bytes(row)); prev = row

    def samples(row):                              # unpack to 0..255 per channel
        if depth == 8:  return list(row)
        if depth == 16: return [row[i] for i in range(0, len(row), 2)]
        per, mask, mx = 8 // depth, (1 << depth) - 1, (1 << depth) - 1
        vals = []
        for byte in row:
            for k in range(per):
                vals.append((byte >> (8 - depth * (k + 1))) & mask)
        return [v if ctype == 3 else v * 255 // mx for v in vals]

    px = []
    for row in out:
        v, line = samples(row), []
        for x in range(w):
            s = v[x * nch:(x + 1) * nch]
            if ctype == 3:
                i = s[0]
                lum = (plte[i * 3] * 299 + plte[i * 3 + 1] * 587 + plte[i * 3 + 2] * 114) // 1000
                alpha = trns[i] if trns and i < len(trns) else 255
            elif ctype in (2, 6):
                lum = (s[0] * 299 + s[1] * 587 + s[2] * 114) // 1000
                alpha = s[3] if ctype == 6 else 255
            else:
                lum, alpha = s[0], (s[1] if ctype == 4 else 255)
            line.append(1 if (alpha >= 128 and lum < 128) else 0)
        px.append(line)
    return px, w, h


def from_exact(path, w=W, h=H):
    """Read back an edited PNG pixel-for-pixel -- no crop, no rescale.

    This is the other half of `--preview`: hand someone the art at 1:1 or at an
    integer zoom, let them push individual pixels, and take it back unchanged.
    Unlike --from-image there is no ink-bbox crop and no box filter, so a stray
    edit at the edge shifts nothing else. At a zoom of k each k*k block votes,
    which absorbs an editor that antialiased a brush stroke.
    """
    px, pw, ph = read_png(path)
    if pw % w or ph % h or pw // w != ph // h:
        raise SystemExit("%s is %dx%d; expected %dx%d or an integer zoom of it"
                         % (path, pw, ph, w, h))
    k = pw // w
    grid = []
    for y in range(h):
        row = ""
        for x in range(w):
            ink = sum(px[y * k + b][x * k + a] for b in range(k) for a in range(k))
            row += "#" if ink * 2 >= k * k else "."
        grid.append(row)
    return grid, w, h


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "--from-exact":
        grid, w, h = from_exact(a[1])
        print('FACE = """\\', file=sys.stderr)
        print("\n".join(grid), file=sys.stderr)
        print('"""', file=sys.stderr)
        emit_c(grid, w, h)
    elif a and a[0] == "--from-image":
        size = (int(a[2]), int(a[3])) if len(a) > 3 else (None, None)
        grid, w, h = from_image(a[1], *size)
        print('FACE = """\\', file=sys.stderr)
        print("\n".join(grid), file=sys.stderr)
        print('"""', file=sys.stderr)
        emit_c(grid, w, h)
    elif len(a) > 1 and a[0] == "--preview":
        emit_png(a[1], scale=int(a[2]) if len(a) > 2 else 6)
    else:
        emit_c()
