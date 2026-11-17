#!/usr/bin/env python3
#
# make_pngs.py - generate the four small test PNGs with the Python standard
# library only (zlib + struct + binascii). No third-party modules, no network.
#
# Images (kept tiny on purpose):
#   nonaligned_rgb.png  7x5   truecolor 8bpp (RGB,     21 B/row -> odd stride)
#   aligned_rgb.png     8x4   truecolor 8bpp (RGB,     24 B/row)
#   alpha_rgba.png      7x5   truecolor+alpha (RGBA,  28 B/row)
#   palette_idx.png     7x5   indexed 8bpp with tRNS palette alpha (7 B/row)
#
import binascii
import os
import struct
import sys
import zlib

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def chunk(tag, data):
    return (
        struct.pack(">I", len(data))
        + tag
        + data
        + struct.pack(">I", binascii.crc32(tag + data) & 0xFFFFFFFF)
    )


def ihdr(width, height, depth, color_type):
    # 8-bit depth, no interlace; filter method 0, compression 0
    return chunk(
        b"IHDR",
        struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, 0),
    )


def idat(raw_rows):
    return chunk(b"IDAT", zlib.compress(b"".join(raw_rows), 9))


def iend():
    return chunk(b"IEND", b"")


def pixel_rgb(x, y):
    # deterministic, row-dependent content so copies can't pass by luck
    return bytes(((x * 17 + y * 7) & 0xFF,
                  (x * 31 + y * 11) & 0xFF,
                  (x * 5 + y * 47 + 1) & 0xFF))


def pixel_rgba(x, y):
    # alpha sweeps 0..255-ish so the alpha byte is exercised
    return pixel_rgb(x, y) + bytes(((x * 33 + y * 13) & 0xFF,))


def write_png(path, width, height, depth, color_type, rows, plte=None, trns=None):
    out = [PNG_SIG, ihdr(width, height, depth, color_type)]
    if plte is not None:
        out.append(chunk(b"PLTE", plte))
    if trns is not None:
        out.append(chunk(b"tRNS", trns))
    out.append(idat(rows))
    out.append(iend())
    with open(path, "wb") as f:
        f.write(b"".join(out))


def main(out_dir):
    os.makedirs(out_dir, exist_ok=True)

    # 1) non-aligned width, RGB
    w, h = 7, 5
    rows = [b"\x00" + b"".join(pixel_rgb(x, y) for x in range(w))
            for y in range(h)]
    write_png(os.path.join(out_dir, "nonaligned_rgb.png"),
              w, h, 8, 2, rows)

    # 2) aligned width, RGB (8 pixels * 3 = 24, multiple of 8/16)
    w, h = 8, 4
    rows = [b"\x00" + b"".join(pixel_rgb(x, y) for x in range(w))
            for y in range(h)]
    write_png(os.path.join(out_dir, "aligned_rgb.png"),
              w, h, 8, 2, rows)

    # 3) RGBA with a real alpha channel
    w, h = 7, 5
    rows = [b"\x00" + b"".join(pixel_rgba(x, y) for x in range(w))
            for y in range(h)]
    write_png(os.path.join(out_dir, "alpha_rgba.png"),
              w, h, 8, 6, rows)

    # 4) 8-bit indexed, 4 palette entries; tRNS gives entry 1 alpha = 0x40
    w, h = 7, 5
    palette = bytes((0x10, 0x20, 0x30,   # entry 0
                     0x40, 0x50, 0x60,   # entry 1 (semi-transparent)
                     0x70, 0x80, 0x90,   # entry 2
                     0xA0, 0xB0, 0xC0))  # entry 3
    trns = bytes((0xFF, 0x40, 0xFF, 0xFF))
    rows = []
    for y in range(h):
        row = bytes((x + y) & 3 for x in range(w))  # indices 0..3
        rows.append(b"\x00" + row)
    write_png(os.path.join(out_dir, "palette_idx.png"),
              w, h, 8, 3, rows, plte=palette, trns=trns)

    for name in sorted(os.listdir(out_dir)):
        if name.endswith(".png"):
            print("generated %-20s %d bytes" %
                  (name, os.path.getsize(os.path.join(out_dir, name))))


if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(__file__)
    main(target)
