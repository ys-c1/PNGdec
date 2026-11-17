#!/usr/bin/env python3
"""Generate tiny, self-contained PNG test fixtures (stdlib only).

Run from anywhere:
    python3 -I make_samples.py <outdir>

Creates one fixture per class needed for the linearization tests:
  srgb_rgba.png     8-bit RGBA, carries sRGB chunk (also varying alpha)
  nogama_rgb.png    8-bit RGB, no sRGB and no gAMA (untagged)
  gama_gray.png     8-bit gray+alpha, carries gAMA only (gamma 1/0.45)
  gama_rgb.png      8-bit RGB, carries gAMA only (gamma 2.2)
  conflict_rgb.png  8-bit RGB, carries BOTH sRGB and gAMA (priority test)
  srgb_pal.png      8-bit indexed with tRNS palette alpha, sRGB chunk

All images are deliberately tiny (a handful of rows) so the whole test
suite runs in one shot.
"""
import os
import struct
import sys
import zlib


def chunk(tag, data):
    out = struct.pack(">I", len(data)) + tag + data
    return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def ihdr(w, h, color_type, bit_depth=8):
    return chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, bit_depth,
                                     color_type, 0, 0, 0))


def png(color_type, raw_rows, extra_chunks=b"", bit_depth=8, palette=None,
        trns=None):
    sig = b"\x89PNG\r\n\x1a\n"
    head = ihdr(len(raw_rows[0]) //
                {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color_type],
                len(raw_rows), color_type, bit_depth)
    plte = chunk(b"PLTE", palette) if palette is not None else b""
    tr = chunk(b"tRNS", trns) if trns is not None else b""
    # filter byte 0 (None) per scanline
    idat = b"".join(b"\x00" + row for row in raw_rows)
    return sig + head + extra_chunks + plte + tr + \
        chunk(b"IDAT", zlib.compress(idat, 9)) + chunk(b"IEND", b"")


def srgb_chunk(rendering_intent=0):
    return chunk(b"sRGB", bytes([rendering_intent]))


def gama_chunk(gamma):  # gamma is the display exponent (e.g. 2.2)
    return chunk(b"gAMA", struct.pack(">I", round(gamma * 100000)))


# A 4x4 RGB row pattern spanning the transfer curve; reused everywhere.
RGB = [
    [(0, 0, 0), (10, 20, 30), (128, 64, 200), (255, 255, 255)],
    [(1, 2, 3), (50, 100, 150), (200, 210, 220), (254, 128, 1)],
    [(18, 52, 86), (105, 171, 37), (233, 241, 249), (77, 77, 77)],
    [(255, 0, 0), (0, 255, 0), (0, 0, 255), (127, 127, 127)],
]
ALPHA = [[255, 128, 64, 0], [32, 200, 255, 16],
         [96, 160, 224, 48], [255, 255, 255, 255]]


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    # 1. sRGB RGBA with real alpha coverage
    rows = [bytes(v for px, a in zip(r, al) for v in (*px, a))
            for r, al in zip(RGB, ALPHA)]
    open(os.path.join(outdir, "srgb_rgba.png"), "wb").write(
        png(6, rows, srgb_chunk()))
    # 2. untagged RGB
    rows = [bytes(v for px in r for v in px) for r in RGB]
    open(os.path.join(outdir, "nogama_rgb.png"), "wb").write(png(2, rows))
    # 3. gAMA-only gray+alpha (gamma = 2.2)
    ga = [(0, 255), (18, 200), (128, 128), (255, 0),
          (64, 64), (200, 32), (32, 16), (96, 240),
          (255, 255), (1, 1), (127, 255), (100, 100),
          (250, 5), (5, 250), (77, 150), (180, 90)]
    rows = [bytes(v for px in r for v in px) for r in
            [[ga[y * 4 + x] for x in range(4)] for y in range(4)]]
    open(os.path.join(outdir, "gama_gray.png"), "wb").write(
        png(4, rows, gama_chunk(2.2)))
    # 4. gAMA-only RGB, non-sRGB exponent
    rows = [bytes(v for px in r for v in px) for r in RGB]
    open(os.path.join(outdir, "gama_rgb.png"), "wb").write(
        png(2, rows, gama_chunk(1.8)))
    # 5. conflict: sRGB AND gAMA present (sRGB must win)
    open(os.path.join(outdir, "conflict_rgb.png"), "wb").write(
        png(2, rows, srgb_chunk() + gama_chunk(1.8)))
    # 6. sRGB indexed, palette carries alpha via tRNS
    pal = bytes(v for c in [(0, 0, 0), (255, 0, 0), (0, 255, 0),
                            (0, 0, 255), (128, 128, 128), (200, 100, 50),
                            (10, 20, 30), (255, 255, 255)] for v in c)
    trns = bytes([255, 128, 64, 0, 200, 32, 16, 255])
    idx = [bytes([0, 1, 2, 3]), bytes([4, 5, 6, 7]),
           bytes([7, 6, 5, 4]), bytes([3, 2, 1, 0])]
    open(os.path.join(outdir, "srgb_pal.png"), "wb").write(
        png(3, idx, srgb_chunk(), bit_depth=8, palette=pal, trns=trns))
    # 7. malformed gAMA (value 0): must be ignored, decode stays plain
    open(os.path.join(outdir, "bad_gama_rgb.png"), "wb").write(
        png(2, rows, chunk(b"gAMA", struct.pack(">I", 0))))
    print("wrote 7 fixtures to", outdir)


if __name__ == "__main__":
    main(sys.argv[1])
