#!/usr/bin/env python3
"""Emit the nine Settings tile icons as LVGL A8 descriptors for palm_icons.c.

24x22 A8 (0 = transparent, 255 = ink), the same frame as icon_guru, so they sit
in the Settings grid on the launcher's own cell geometry without a colour-format
conversion. W2 of the Settings phase; the tiles they belong to are the table in
docs/BACKLOG.md under the W phase.

ALL NINE IN ONE FILE, deliberately. The two icons that came before this
(gen_guru_icon.py, gen_zip_icon.py) are a script each, which was right for one
icon and would be wrong for nine: these share a drawing toolkit and, far more
importantly, they share a LOOK. Nine separate files drift -- one author picks a
7.4 disk and the next picks 8.6, and the grid ends up with icons that are
individually fine and collectively a jumble. The disk is defined once, at the
top, and every tile is knocked out of the same one.

THE HOUSE IDIOM, inherited from the PumpkinOS originals and documented at length
in gen_guru_icon.py: a Palm launcher icon is a SOLID DISK with the subject
knocked out of it in WHITE. The disk is the ink; the subject is the hole. Detail
is then painted back IN ink inside the hole, which is what stops a knocked-out
shape reading as a blob. Drawing the subject as ink on nothing is the obvious
approach and it is the wrong one -- it reads as spindly line art next to the
solid originals, and at 24x22 it loses to the panel.

Two consequences worth knowing before editing:

  * Knocking out (v=0) OUTSIDE the disk is free -- it writes 0 over 0. So no
    shape needs clipping to the disk; overhang simply doesn't render.
  * Painting back IN (v=255) outside the disk is NOT free -- it spills ink into
    the transparent margin and the icon grows a wart. Every ink-back call here
    is sized to stay inside.

The disk is R=8.6, not icon_guru's 7.4: the guru icon is one shape in a big
field, and these carry interior detail (a clock's hands, a newspaper's rules)
that needs the room. Compare against icon_datebook, whose disk very nearly
fills the 25px frame -- 8.6 sits between the two.

    python3 tools/gen_settings_icons.py            # the C, to paste into palm_icons.c
    python3 tools/gen_settings_icons.py --preview  # ASCII, to actually look at them
"""
import math
import sys

W, H = 24, 22
CX, CY = 12.0, 11.0
DISK_R = 8.6


# ---------------------------------------------------------------- toolkit ----
# Every primitive takes v: 255 paints ink, 0 knocks a hole. That symmetry is the
# whole point -- the same disc() draws the ink disk and the white clock face.

def new():
    return [[0] * W for _ in range(H)]


def px(g, x, y, v):
    if 0 <= x < W and 0 <= y < H:
        g[y][x] = v


def disc(g, cx, cy, r, v=255):
    for j in range(H):
        for i in range(W):
            if (i - cx) ** 2 + (j - cy) ** 2 <= r * r:
                g[j][i] = v


def rect(g, x0, y0, x1, y1, v=255):
    for j in range(int(y0), int(y1) + 1):
        for i in range(int(x0), int(x1) + 1):
            px(g, i, j, v)


def band(g, cx, cy, r, t, a0, a1, v=255):
    """An arc with THICKNESS, tested per-pixel rather than stepped along the curve.

    gen_guru_icon.py steps its aura by angle and sets single pixels, which is
    right for a 1px hairline. A knocked-out arc has to be at least 2px wide to
    survive the panel, and stepping a thick arc means overdrawing it many times
    and still leaving holes where the curvature is steep. Testing every pixel
    against a radius window and an angle window is slower, cares not at all
    about curvature, and at 24x22 'slower' is meaningless.

    Angles are in SCREEN space: y grows downward, so atan2 measures clockwise
    and 270 is straight up. Every arc below is written with that in mind."""
    span = (a1 - a0) % 360.0
    if span == 0.0:
        span = 360.0                      # a0 == a1 means a full ring
    for j in range(H):
        for i in range(W):
            dx, dy = i - cx, j - cy
            if abs(math.hypot(dx, dy) - r) > t / 2.0:
                continue
            if (math.degrees(math.atan2(dy, dx)) - a0) % 360.0 <= span:
                g[j][i] = v


def line(g, x0, y0, x1, y1, t=1.0, v=255):
    """A thick segment, by perpendicular distance. Same reasoning as band()."""
    for j in range(H):
        for i in range(W):
            vx, vy = x1 - x0, y1 - y0
            L2 = vx * vx + vy * vy
            if L2 == 0:
                continue
            s = max(0.0, min(1.0, ((i - x0) * vx + (j - y0) * vy) / L2))
            if math.hypot(i - (x0 + s * vx), j - (y0 + s * vy)) <= t / 2.0:
                g[j][i] = v


def tri(g, pts, v=255):
    """Filled triangle, by barycentric sign test -- for arrowheads and the pin."""
    (ax, ay), (bx, by), (cx_, cy_) = pts

    def side(px_, py_, x0, y0, x1, y1):
        return (x1 - x0) * (py_ - y0) - (y1 - y0) * (px_ - x0)

    for j in range(H):
        for i in range(W):
            d1 = side(i, j, ax, ay, bx, by)
            d2 = side(i, j, bx, by, cx_, cy_)
            d3 = side(i, j, cx_, cy_, ax, ay)
            if not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0)):
                g[j][i] = v


def disk():
    """Every tile starts here: the shared ink disk."""
    g = new()
    disc(g, CX, CY, DISK_R, 255)
    return g


def clip(g):
    """Erase everything outside the disk. Applied to every tile on the way out.

    The hazard is written up top -- painting ink back in outside the disk grows
    a wart -- and it was walked into within the hour, by the Owner tile: the ink
    gap that keeps its head from merging with its shoulders is a full-width row,
    and a full-width row is exactly the thing that does not stop at the disk
    edge. It rendered as a bar straight through the icon.

    The fix is here rather than in the tile because 'remember to clip' is not a
    rule that survives the ninth icon. Tiles may now draw as carelessly as they
    like outside the disk; the disk is the frame, and this enforces it."""
    for j in range(H):
        for i in range(W):
            if (i - CX) ** 2 + (j - CY) ** 2 > DISK_R * DISK_R:
                g[j][i] = 0
    return g


# ------------------------------------------------------------------ tiles ----
# Order matches the nine-tile table in docs/BACKLOG.md, which is the order they
# are laid out on screen. Keep them in step.

def t_wifi():
    """Three arcs opening upward over a dot -- the universal fan.

    TWO arcs, not the three the real mark has. Three was tried and the disk is
    not big enough to hold them: a knocked arc needs ~2px to survive the panel,
    and three of those plus the two ink gaps between them plus the dot comes to
    more than the ~9px of radius there is to spend. The third arc ate the last
    gap, the whites merged, and the fan read as a crescent moon.

    The origin sits LOW in the disk (y=16.2) rather than at its centre, so both
    arcs fit above it. Centring the fan runs the outer arc off the top of the
    disk, where it is cropped into two disconnected commas."""
    g = disk()
    for r in (3.8, 7.2):
        band(g, 12, 16.2, r, 1.9, 208, 332, 0)
    disc(g, 12, 15.6, 1.35, 0)
    return g


def t_accounts():
    """A key: bow, shaft, two teeth.

    NOT a padlock, and not a person. A padlock says 'locked' (the lock screen
    already owns that idea) and a person collides with the Owner tile three
    cells away. A key says 'the credential that opens the account', which is
    exactly what this wizard collects."""
    g = disk()
    band(g, 8.5, 11, 3.2, 2.2, 0, 359, 0)   # the bow
    disc(g, 8.5, 11, 1.4, 255)              # ...its hole, painted back in
    rect(g, 11, 10, 18, 11, 0)              # the shaft
    rect(g, 15, 12, 16, 15, 0)              # two teeth, unequal, so it reads
    rect(g, 17, 12, 18, 14, 0)              # as a key and not as a comb
    return g


def t_news():
    """A page with rules. The rules are ink painted back into the white page.

    The page is x7..16 by y6..16, which is as large as it can be and still leave
    ~2px of ink on every side. Sized any bigger it stops being a page ON a disk
    and becomes a page that has EATEN the disk -- the first draft was x5..18 by
    y4..18 and left four disconnected ink crumbs at the corners."""
    g = disk()
    rect(g, 7, 6, 16, 16, 0)
    rect(g, 8, 7, 15, 8, 255)               # masthead: a heavier first rule
    for y in (10, 12, 14):
        rect(g, 8, y, 15, y, 255)
    return g


def t_datetime():
    """A clock face, hands painted back in.

    Hands at 10:10 -- the watchmaker's pose. It is not decoration: at this size
    hands near the vertical or the horizontal merge with the face's edge, and
    10:10 puts both on clean diagonals where each gets pixels of its own."""
    g = disk()
    disc(g, CX, CY, 6.0, 0)                          # face: leaves a 2.6px bezel
    line(g, CX, CY, CX - 3.0, CY - 2.6, 1.6, 255)    # hour
    line(g, CX, CY, CX + 3.4, CY - 3.1, 1.4, 255)    # minute
    disc(g, CX, CY, 1.0, 255)                        # the boss, which joins them
    return g


def t_display():
    """A sun. The wizard behind this tile is brightness and backlight timeout,
    so it wants light, not display hardware.

    A SCREEN ON A STAND WITH A SUN LIT INSIDE IT was the first design and is a
    better idea than it is an icon. A screen wide enough to read as a screen is
    ~10px across, which at y=5 is the full width the disk has to give -- so the
    screen's top edge lands flat on the disk's crown and shears it off, and the
    tile stops being a disk at all. Pushed down far enough to spare the crown,
    the screen has no room left inside it for the sun that was the entire point.

    The collision it was drawn to avoid turns out not to be real: the Wi-Fi fan
    two cells away is CONCENTRIC ARCS and this is RADIAL SPOKES, which is the
    same distinction gen_guru_icon.py leans on to keep her aura from reading as
    eyelashes. Arcs and spokes do not look alike, even at 24x22.

    The ray gap is the measurement that matters. At body 3.4 / rays from 4.7 the
    ink between them is 1.3px, and a 1.7px-wide ray simply bridges it -- the sun
    fused into an asterisk and the centre row ran white from edge to edge. Body
    3.0 with rays from 5.0 leaves 2px, which holds."""
    g = disk()
    disc(g, CX, CY, 3.5, 0)                 # the body
    for a in range(0, 360, 45):             # eight rays, clear of the body
        rr = math.radians(a)                # THIN and long, not short and fat:
        line(g, CX + math.cos(rr) * 5.3, CY + math.sin(rr) * 5.3,   # fat rays with
                CX + math.cos(rr) * 7.1, CY + math.sin(rr) * 7.1, 1.2, 0)  # narrow
                                            # gaps read as a gear, not a sun
    return g


def t_location():
    """A map pin: round head, tapered point, hole punched back through.

    The hole is the whole trick. A solid white teardrop reads as a balloon; the
    punched hole is what makes it a pin, because it is how every map pin ever
    drawn shows that the head is a ring seen face-on."""
    g = disk()
    disc(g, 12, 8.5, 4.4, 0)
    tri(g, [(8.2, 11.0), (15.8, 11.0), (12.0, 18.5)], 0)
    disc(g, 12, 8.5, 1.7, 255)
    return g


def t_sync():
    """Two opposed straight arrows: one down, one up. Records going both ways.

    TWO CHASING ARCS were the first design -- the round-trip mark everyone
    draws -- and they do not work here. A ring knocked at r=5.6 has its only
    surviving ink at the far left and far right, where the arcs' gaps fall; the
    arrowheads then have to sit at those same ends, so they eat the last two ink
    bridges and the whole interior floods white. Straight arrows put their heads
    at the TOP and BOTTOM, where there is ink to spare, and leave the disk's
    waist untouched.

    It also stops this colliding with icon_hotsync in the launcher, which keeps
    the circular mark. This tile is the sync SETTINGS -- conflict policy, which
    collection is which -- and wants to be a relative, not a twin."""
    g = disk()
    rect(g, 8, 5, 9, 13, 0)                                # left shaft, down
    tri(g, [(6.0, 12.5), (11.5, 12.5), (8.75, 17.5)], 0)
    rect(g, 14, 9, 15, 17, 0)                              # right shaft, up
    tri(g, [(12.5, 9.5), (18.0, 9.5), (15.25, 4.5)], 0)
    return g


def t_owner():
    """A bust: head and shoulders.

    Two measurements, and both were got wrong first time.

    The gap between head and shoulders has to be INK and at least 2px, or the
    two whites bleed into a single mushroom.

    The shoulders must not reach the disk's edge until they are BELOW its
    waist. A 7.2 disc centred at y=20 is still 3px wide at y=14, where the
    disk is at its fullest -- so the white met the rim high up, the ink rim
    broke, and the tile read as a bowl rather than a bust. 5.2 at y=19.2 stays
    clear of the rim until y=18, by which point the disk is closing anyway and
    the white filling it is exactly what shoulders are supposed to do."""
    g = disk()
    disc(g, 12, 8.6, 2.9, 0)
    disc(g, 12, 19.2, 5.2, 0)               # shoulders: a disc, mostly below
    rect(g, 0, 12, W - 1, 13, 255)          # ...and the ink gap that separates them,
                                            #    2px, drawn full-width and clipped
    return g


def t_about():
    """A lower-case i. The one tile that is a letterform, because 'information'
    has no picture that beats it at 24x22."""
    g = disk()
    disc(g, 12, 5.8, 1.6, 0)                # the tittle
    rect(g, 10, 9, 13, 17, 0)               # the stem
    return g


TILES = [
    ("icon_set_wifi",     "Wi-Fi",       t_wifi),
    ("icon_set_accounts", "Accounts",    t_accounts),
    ("icon_set_news",     "News",        t_news),
    ("icon_set_datetime", "Date & Time", t_datetime),
    ("icon_set_display",  "Display",     t_display),
    ("icon_set_location", "Location",    t_location),
    ("icon_set_sync",     "Sync",        t_sync),
    ("icon_set_owner",    "Owner",       t_owner),
    ("icon_set_about",    "About",       t_about),
]


def main():
    if "--preview" in sys.argv:
        for name, label, fn in TILES:
            g = clip(fn())
            print("--- %s (%s) ---" % (label, name))
            for row in g:
                print("".join("#" if v > 127 else ("+" if v else ".") for v in row))
            print()
        return

    print("/* the nine Settings tile icons -- tools/gen_settings_icons.py */")
    for name, label, fn in TILES:
        g = clip(fn())
        flat = ",".join(str(v) for row in g for v in row)
        print("/* %s */" % label)
        print("static const uint8_t %s_map[] = {%s};" % (name, flat))
        print("const lv_image_dsc_t %s = {" % name)
        print("  .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,.flags=0,"
              ".w=%d,.h=%d,.stride=%d,.reserved_2=0}," % (W, H, W))
        print("  .data_size=%d,.data=%s_map };" % (W * H, name))


if __name__ == "__main__":
    main()
