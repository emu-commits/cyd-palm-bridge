#!/usr/bin/env python3
"""ppm2png.py -- convert the sim's P6 PPM screenshots to PNG (stdlib only).

Usage: ppm2png.py file.ppm [file2.ppm ...]   -> writes file.png next to each.
"""
import struct, sys, zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6, whitespace/comment-tolerant header: magic, width, height, maxval
    tok, i = [], 0
    while len(tok) < 4:
        while i < len(data) and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b"#":
            while i < len(data) and data[i:i+1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j:j+1].isspace():
            j += 1
        tok.append(data[i:j]); i = j
    i += 1  # single whitespace after maxval
    if tok[0] != b"P6":
        raise ValueError(f"{path}: not a P6 PPM")
    w, h = int(tok[1]), int(tok[2])
    return w, h, data[i:i + w * h * 3]


def write_png(path, w, h, rgb):
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 6))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for src in sys.argv[1:]:
        w, h, rgb = read_ppm(src)
        dst = src.rsplit(".", 1)[0] + ".png"
        write_png(dst, w, h, rgb)
        print(dst)
