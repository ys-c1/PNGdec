#!/usr/bin/env python3
"""Generate tiny PNG samples used by the indexed-output tests.

Only the Python standard library is used (zlib/struct). Each file is
deliberately small: a handful of pixels, no interlacing, no 16-bit samples.

Written next to this script:
  samples/idx1.png   1-bit palette, 2 entries, no tRNS
  samples/idx2.png   2-bit palette, 4 entries, tRNS (alpha 255/0/128/255)
  samples/idx4.png   4-bit palette, 16 entries, no tRNS
  samples/idx8.png   8-bit palette, 256 entries, tRNS
  samples/true.png   24-bit truecolor (not a palette image)
  samples/gray.png   8-bit grayscale (not a palette image)
  samples/expected.txt  machine readable pixels/palette, consumed by driver.cpp
  samples/samples.html  human readable dump
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "samples")


def chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def write_png(name, width, height, bit_depth, color_type, rows,
              palette=None, trns=None):
    """rows: packed scanline payloads (the filter byte is added here)."""
    raw = b"".join(b"\x00" + r for r in rows)
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, bit_depth,
                                       color_type, 0, 0, 0))
    if palette:
        data += chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    if trns is not None:
        data += chunk(b"tRNS", bytes(trns))
    data += chunk(b"IDAT", zlib.compress(raw, 9))
    data += chunk(b"IEND", b"")
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)


def pack_bits(values, bit_depth):
    out = bytearray()
    acc = used = 0
    for v in values:
        acc = (acc << bit_depth) | v
        used += bit_depth
        if used == 8:
            out.append(acc)
            acc = used = 0
    if used:
        out.append(acc << (8 - used))
    return bytes(out)


def main():
    os.makedirs(OUT, exist_ok=True)
    report = []

    # ---------------- palette images ----------------
    w, h = 7, 3
    pal1 = [(0, 0, 0), (255, 255, 255)]
    idx1 = [[(x + y) & 1 for x in range(w)] for y in range(h)]
    write_png("idx1.png", w, h, 1, 3, [pack_bits(r, 1) for r in idx1], pal1)
    report.append((("idx1.png", w, h, 1, 3, idx1, pal1, None)))

    w, h = 5, 3
    pal2 = [(10, 20, 30), (200, 100, 50), (1, 250, 3), (77, 77, 77)]
    trns2 = [255, 0, 128, 255]
    idx2 = [[(x + y * 2) & 3 for x in range(w)] for y in range(h)]
    write_png("idx2.png", w, h, 2, 3, [pack_bits(r, 2) for r in idx2],
              pal2, trns=trns2)
    report.append((("idx2.png", w, h, 2, 3, idx2, pal2, trns2)))

    w, h = 9, 3
    pal4 = [((i * 16) & 0xFF, (255 - i * 16) & 0xFF, (i * 8) & 0xFF)
            for i in range(16)]
    idx4 = [[(x + y * 3 + 1) & 15 for x in range(w)] for y in range(h)]
    write_png("idx4.png", w, h, 4, 3, [pack_bits(r, 4) for r in idx4], pal4)
    report.append((("idx4.png", w, h, 4, 3, idx4, pal4, None)))

    w, h = 6, 4
    pal8 = [(i, (i * 3) & 0xFF, (255 - i) & 0xFF) for i in range(256)]
    trns8 = [255] * 256
    trns8[0] = 0
    trns8[9] = 128
    idx8 = [[(x * 37 + y * 53) & 0xFF for x in range(w)] for y in range(h)]
    write_png("idx8.png", w, h, 8, 3, [bytes(r) for r in idx8],
              pal8, trns=trns8)
    report.append((("idx8.png", w, h, 8, 3, idx8, pal8, trns8)))

    # ---------------- non-palette images ----------------
    w, h = 4, 2
    rgb = [[(x * 60, y * 90, (x + y) * 40) for x in range(w)] for y in range(h)]
    write_png("true.png", w, h, 8, 2,
              [bytes(c for px in row for c in px) for row in rgb])
    report.append((("true.png", w, h, 8, 2, rgb, None, None)))

    w, h = 4, 2
    gray = [[(x * 30 + y * 10) & 0xFF for x in range(w)] for y in range(h)]
    write_png("gray.png", w, h, 8, 0, [bytes(r) for r in gray])
    report.append((("gray.png", w, h, 8, 0,
                    [[(v, v, v) for v in row] for row in gray], None, None)))

    # ---------------- larger samples purely for timing ----------------
    # 160x120 keeps each file a few KB while giving decode times well above
    # clock granularity.
    pw, ph = 160, 120
    perf1 = [[(x + y) & 1 for x in range(pw)] for y in range(ph)]
    write_png("perf_idx1.png", pw, ph, 1, 3,
              [pack_bits(r, 1) for r in perf1], pal1)
    perf2 = [[(x + y * 2) & 3 for x in range(pw)] for y in range(ph)]
    write_png("perf_idx2.png", pw, ph, 2, 3,
              [pack_bits(r, 2) for r in perf2], pal2, trns=trns2)
    perf4 = [[(x + y * 3 + 1) & 15 for x in range(pw)] for y in range(ph)]
    write_png("perf_idx4.png", pw, ph, 4, 3,
              [pack_bits(r, 4) for r in perf4], pal4)
    perf8 = [[(x * 37 + y * 53) & 0xFF for x in range(pw)] for y in range(ph)]
    write_png("perf_idx8.png", pw, ph, 8, 3, [bytes(r) for r in perf8],
              pal8, trns=trns8)
    # a repeating pattern keeps this truecolor file small while staying big
    # enough (160x120) for a measurable decode time
    perf_true = [[bytes([((x // 4) * 16) & 0xFF, ((y // 4) * 16) & 0xFF,
                        (((x + y) // 8) * 32) & 0xFF]) for x in range(pw)]
                 for y in range(ph)]
    write_png("perf_true.png", pw, ph, 8, 2,
              [b"".join(row) for row in perf_true])

    # ---------------- machine-readable expectations ----------------
    with open(os.path.join(OUT, "expected.txt"), "w", encoding="utf-8") as f:
        for name, w, h, bits, ctype, pixels, pal, trns in report:
            if pal is None:
                f.write("%s %d %d %d %d NOPAL 0 0\n" % (name, w, h, bits, ctype))
                for row in pixels:
                    f.write(" ".join("%d,%d,%d" % tuple(v) for v in row) + "\n")
            else:
                f.write("%s %d %d %d %d PAL %d %d\n" %
                        (name, w, h, bits, ctype, len(pal),
                         1 if trns is not None else 0))
                for i, (r, g, b) in enumerate(pal):
                    a = trns[i] if trns is not None and i < len(trns) else 255
                    f.write("%d %d %d %d\n" % (r, g, b, a))
                for row in pixels:
                    f.write(" ".join(str(v) for v in row) + "\n")

    # ---------------- readable dump ----------------
    lines = ["<!doctype html><meta charset=utf-8>",
             "<title>indexed samples</title>",
             "<h1>pngdec indexed-output samples</h1>"]
    for name, w, h, bits, ctype, pixels, pal, trns in report:
        lines.append("<h2>%s (%dx%d, %d-bit, color type %d%s)</h2>" %
                     (name, w, h, bits, ctype,
                      ", palette" if pal else ", no palette"))
        if pal:
            trans = [i for i in range(len(pal))
                     if trns is not None and i < len(trns) and trns[i] != 255]
            lines.append("<p>PLTE: %d entries; tRNS transparent indexes: %s</p>" %
                         (len(pal), trans if trans else "none"))
        lines.append("<table border=1>")
        for row in pixels:
            cells = []
            for v in row:
                if pal is None:
                    cells.append("<td>rgb(%d,%d,%d)</td>" % tuple(v))
                else:
                    r, g, b = pal[v]
                    a = trns[v] if trns is not None and v < len(trns) else 255
                    cells.append('<td style="background:rgba(%d,%d,%d,%d)">'
                                 '%d</td>' % (r, g, b, a / 255.0, v))
            lines.append("<tr>" + "".join(cells) + "</tr>")
        lines.append("</table>")
    with open(os.path.join(OUT, "samples.html"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print("wrote samples to", OUT)


if __name__ == "__main__":
    main()
