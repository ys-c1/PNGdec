#!/usr/bin/env python3
# Generate tiny valid PNG samples for the header-cache benchmark.
# Standard library only (zlib/struct) - no third-party packages.
# Run with: python3 -I gen_samples.py <output_dir>
import os
import struct
import sys
import zlib


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def build_png(path, width, height, color_type, bitdepth, raw_rows,
              palette=None, trns=None, text=None):
    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, bitdepth, color_type, 0, 0, 0)
    out = sig + chunk(b"IHDR", ihdr)
    if text is not None:
        # tEXt payload provides fixed-length, in-place mutable bytes for the
        # fingerprint invalidation tests.
        out += chunk(b"tEXt", b"Comment\x00" + text)
    if palette is not None:
        out += chunk(b"PLTE", b"".join(struct.pack("BBB", *c) for c in palette))
    if trns is not None:
        out += chunk(b"tRNS", bytes(trns))
    raw = b"".join(b"\x00" + row for row in raw_rows)  # filter type 0
    out += chunk(b"IDAT", zlib.compress(raw, 9))
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)
    return out


def rgb_rows(width, height, seed):
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            row += bytes([(x * 17 + seed) & 0xFF,
                          (y * 31 + seed) & 0xFF,
                          (x + y + seed) & 0xFF])
        rows.append(bytes(row))
    return rows


def index_rows(width, height, ncolors, seed):
    return [bytes(((x + y + seed) % ncolors) for x in range(width))
            for y in range(height)]


def gray_rows(width, height, seed):
    return [bytes(((x * 16 + y * 4 + seed) & 0xFF) for x in range(width))
            for y in range(height)]


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for stale in os.listdir(outdir):
        os.remove(os.path.join(outdir, stale))
    w, h = 16, 12

    # A: 24-bit truecolor with a tEXt chunk carrying mutable payload bytes.
    text = b"cache bench sample A, mutable text payload 0000000000"
    build_png(os.path.join(outdir, "a_rgb.png"), w, h, 2, 8,
              rgb_rows(w, h, 1), text=text)

    # B: 8-bit indexed with PLTE + tRNS (alpha palette exercised)
    pal4 = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
    build_png(os.path.join(outdir, "b_pal8.png"), w, h, 3, 8,
              index_rows(w, h, 4, 0), palette=pal4, trns=[0, 255, 128, 255])

    # C: 8-bit grayscale
    build_png(os.path.join(outdir, "c_gray.png"), w, h, 0, 8,
              gray_rows(w, h, 3))

    # D: 4-bit indexed, 16 palette entries
    pal16 = [((i * 16) & 0xFF, (i * 8) & 0xFF, (i * 4) & 0xFF)
             for i in range(16)]
    rows4 = []
    for y in range(h):
        row = bytearray()
        for x in range(0, w, 2):
            hi = ((x + y) % 16) << 4
            lo = (x + y + 1) % 16
            row.append(hi | lo)
        rows4.append(bytes(row))
    build_png(os.path.join(outdir, "d_pal4.png"), w, h, 3, 4,
              rows4, palette=pal16)

    # E: larger indexed sample to show the header parse/hash cost scales with
    # header size, not with compressed pixel payload size.
    we, he = 96, 96
    pal256 = [((i * 7) & 0xFF, (i * 11) & 0xFF, (i * 13) & 0xFF)
              for i in range(256)]
    rows_e = [bytes(((x * 3 + y * 5) & 0xFF) for x in range(we))
              for y in range(he)]
    build_png(os.path.join(outdir, "e_pal8_big.png"), we, he, 3, 8,
              rows_e, palette=pal256)

    sizes = {n: os.path.getsize(os.path.join(outdir, n))
             for n in sorted(os.listdir(outdir))}
    total = sum(sizes.values())
    for n, s in sizes.items():
        print("  %-14s %4d bytes" % (n, s))
    print("generated %d files, %d bytes total" % (len(sizes), total))


if __name__ == "__main__":
    main()
