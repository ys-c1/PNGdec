#!/usr/bin/env python3
"""
Generate tiny synthetic PNG samples + decode oracle data for the
PNGdec provenance tests. Pure Python standard library (zlib/struct/
binascii) - no third-party packages, no network access.

Outputs (in the directory given as argv[1]):
  *.png     synthetic encoded images
  *.raw     expected framebuffer contents (iPitch * height bytes)
  *.tr      expected per-pixel transparent flags, in DECODE EMISSION ORDER
            (raster order for non-interlaced, Adam7 pass-major for interlaced)
  *.prov    one line per emitted scanline: "filter pass srcrow npixels"
  manifest.txt   one line per image:
            name,width,height,colortype,bpp,interlaced
"""
import binascii
import os
import struct
import sys
import zlib

# Adam7 tables - must match src/png.inl
A7_START = [(0, 0), (4, 0), (0, 4), (2, 0), (0, 2), (1, 0), (0, 1)]
A7_STEP = [(8, 8), (8, 8), (4, 8), (4, 4), (2, 4), (2, 2), (1, 2)]

FILTERS = (0, 1, 2, 3, 4)  # None, Sub, Up, Average, Paeth


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def encode_row(raw, prev, filt, bpp):
    """Return filtered scanline bytes (without the leading filter byte)."""
    n = len(raw)
    out = bytearray(n)
    for i in range(n):
        x = raw[i]
        a = raw[i - bpp] if i >= bpp else 0
        b = prev[i] if prev is not None else 0
        c = prev[i - bpp] if (prev is not None and i >= bpp) else 0
        if filt == 0:
            v = x
        elif filt == 1:
            v = x - a
        elif filt == 2:
            v = x - b
        elif filt == 3:
            v = x - ((a + b) >> 1)
        else:
            v = x - paeth(a, b, c)
        out[i] = v & 0xFF
    return bytes(out)


def chunk(typ, data):
    out = struct.pack(">I", len(data)) + typ + data
    return out + struct.pack(">I", binascii.crc32(typ + data) & 0xFFFFFFFF)


def write_png(path, w, h, colortype, bpp, interlaced, raw_rows,
              palette=None, trns=None):
    """raw_rows: list of byte rows, each row already iPitch bytes long."""
    sig = b"\x89PNG\r\n\x1a\n"
    depth = {0: bpp, 2: 8, 3: bpp, 4: 8, 6: 8}[colortype]
    ihdr = struct.pack(">IIBBBBB", w, h, depth, colortype, 0, 0,
                       1 if interlaced else 0)
    blocks = []
    if not interlaced:
        for y, row in enumerate(raw_rows):
            filt = FILTERS[y % len(FILTERS)]
            prev = raw_rows[y - 1] if y else None
            blocks.append(bytes([filt]) + encode_row(row, prev, filt,
                                                      row_bpp(colortype, bpp)))
    else:
        for p in range(7):
            sx, sy = A7_STEP[p][0], A7_STEP[p][1]
            ox, oy = A7_START[p]
            xs = list(range(ox, w, sx))
            ys = list(range(oy, h, sy))
            if not xs or not ys:
                continue
            prev = None
            for ry, fy in enumerate(ys):
                sub = extract_sub_row(raw_rows[fy], xs, colortype, bpp, w)
                filt = FILTERS[(ry + p) % len(FILTERS)]
                blocks.append(bytes([filt]) +
                              encode_row(sub, prev, filt,
                                         row_bpp(colortype, bpp)))
                prev = sub
    idat = zlib.compress(b"".join(blocks), 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        if palette is not None:
            f.write(chunk(b"PLTE", palette))
        if trns is not None:
            f.write(chunk(b"tRNS", trns))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def row_bpp(colortype, bpp):
    if colortype in (0, 3):
        return 1
    return {2: 3, 4: 2, 6: 4}[colortype]


def extract_sub_row(full_row, xs, colortype, bpp, w):
    if colortype in (0, 3) and bpp < 8:
        # packed sub-byte pixels: unpack indices then re-pack
        vals = []
        for x in xs:
            shift = 8 - bpp - (x * bpp % 8)
            vals.append((full_row[(x * bpp) >> 3] >> shift) & ((1 << bpp) - 1))
        out = bytearray((len(vals) * bpp + 7) // 8)
        for i, v in enumerate(vals):
            shift = 8 - bpp - (i * bpp % 8)
            out[(i * bpp) >> 3] |= v << shift
        return bytes(out)
    if colortype in (0, 3):
        return bytes(full_row[x] for x in xs)
    n = row_bpp(colortype, bpp)
    return b"".join(bytes(full_row[x * n:x * n + n]) for x in xs)


def pitch_of(w, colortype, bpp):
    if colortype in (0, 3):
        return (w * bpp + 7) // 8
    return w * row_bpp(colortype, bpp)


def emission_coords(w, h, interlaced):
    """Yield (fullx, fully, pass, srcrow) in decoder emission order."""
    if not interlaced:
        for y in range(h):
            for x in range(w):
                yield x, y, 0, y
        return
    for p in range(7):
        sx, sy = A7_STEP[p]
        ox, oy = A7_START[p]
        xs = list(range(ox, w, sx))
        ys = list(range(oy, h, sy))
        if not xs or not ys:
            continue
        for ry, fy in enumerate(ys):
            for fx in xs:
                yield fx, fy, p + 1, ry


def prov_lines(w, h, interlaced):
    """Return expected (filter, pass, srcrow, npixels) per emitted line."""
    lines = []
    if not interlaced:
        for y in range(h):
            lines.append((FILTERS[y % 5], 0, y, w))
        return lines
    for p in range(7):
        sx, sy = A7_STEP[p]
        ox, oy = A7_START[p]
        xs = list(range(ox, w, sx))
        ys = list(range(oy, h, sy))
        if not xs or not ys:
            continue
        for ry in range(len(ys)):
            lines.append((FILTERS[(ry + p) % 5], p + 1, ry, len(xs)))
    return lines


# ---- pixel value / transparency oracles for each sample ----------------

def gray8_pixel(x, y):
    return (x * 17 + y * 31) & 0xFF


def rgb_pixel(x, y):
    return bytes([(x * 23) & 0xFF, (y * 29) & 0xFF, ((x + y) * 13) & 0xFF])


def pal_index(x, y):
    return (x + 2 * y) & 3


def rgba_pixel(x, y):
    s = (x + y) % 3
    a = 0 if s == 0 else (128 if s == 2 else 255)
    return bytes([(x * 31 + 7) & 0xFF, (y * 19 + 5) & 0xFF,
                  (x * 11 + y * 7) & 0xFF, a])


PALETTE = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0])
TRNS4 = bytes([255, 0, 255, 128])  # palette index 1 fully transparent


def transparent(colortype, x, y):
    if colortype == 6:
        return rgba_pixel(x, y)[3] == 0
    if colortype == 3:
        return pal_index(x, y) == 1 and TRNS4[pal_index(x, y)] == 0
    return 0


def build_rows(w, h, colortype, bpp):
    pitch = pitch_of(w, colortype, bpp)
    rows = []
    for y in range(h):
        row = bytearray(pitch)
        for x in range(w):
            if colortype == 0:
                row[x] = gray8_pixel(x, y)
            elif colortype == 2:
                row[x * 3:x * 3 + 3] = rgb_pixel(x, y)
            elif colortype == 3:
                idx = pal_index(x, y)
                if bpp == 8:
                    row[x] = idx
                else:
                    shift = 8 - bpp - (x * bpp % 8)
                    row[(x * bpp) >> 3] |= idx << shift
            elif colortype == 6:
                row[x * 4:x * 4 + 4] = rgba_pixel(x, y)
        rows.append(bytes(row))
    return rows


def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    samples = [
        # name, w, h, colortype, bpp, interlaced, palette, trns
        ("filters_gray8", 9, 7, 0, 8, 0, None, None),
        ("interlace_rgb", 9, 7, 2, 8, 1, None, None),
        ("palette8_trns", 9, 7, 3, 8, 0, PALETTE, TRNS4),
        ("alpha_rgba", 8, 6, 6, 8, 0, None, None),
        ("interlace_pal2", 9, 7, 3, 2, 1, PALETTE, TRNS4),
        ("timing_rgba", 64, 48, 6, 8, 0, None, None),
    ]
    manifest = []
    for name, w, h, ct, bpp, il, pal, trns in samples:
        rows = build_rows(w, h, ct, bpp)
        png_path = os.path.join(outdir, name + ".png")
        write_png(png_path, w, h, ct, bpp, il, rows, pal, trns)
        with open(os.path.join(outdir, name + ".raw"), "wb") as f:
            f.write(b"".join(rows))
        with open(os.path.join(outdir, name + ".tr"), "wb") as f:
            f.write(bytes(transparent(ct, x, y)
                          for x, y, _, _ in emission_coords(w, h, il)))
        with open(os.path.join(outdir, name + ".prov"), "w") as f:
            for filt, passno, srcrow, n in prov_lines(w, h, il):
                f.write(f"{filt} {passno} {srcrow} {n}\n")
        manifest.append(f"{name},{w},{h},{ct},{bpp},{il}")
    with open(os.path.join(outdir, "manifest.txt"), "w") as f:
        f.write("\n".join(manifest) + "\n")
    print(f"generated {len(samples)} samples in {outdir}")


if __name__ == "__main__":
    main()
