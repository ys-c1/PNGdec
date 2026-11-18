#!/usr/bin/env python3
"""Generate small PNG test fixtures using only the Python standard library.

Usage: gen_fixtures.py <outdir>

Fixtures (all 8-bit, non-interlaced except the interlaced one):
  gray8_1024x768.png    - grayscale,        filters cycle 0..4 per row
  rgb8_1024x768.png     - truecolor,        filters cycle 0..4 per row
  pal8_1024x768.png     - indexed + tRNS,   filters cycle 0..4 per row
  rgba8_1024x768.png    - truecolor+alpha,  filters cycle 0..4 per row
  subonly_128x128.png   - truecolor, only None/Sub filters (ring-size-1 OK)
  interlaced_128x128.png- grayscale, IHDR interlace flag set (Adam7)
"""
import zlib, struct, binascii, os, sys

def chunk(typ, data):
    return (struct.pack(">I", len(data)) + typ + data +
            struct.pack(">I", binascii.crc32(typ + data) & 0xffffffff))

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c

def filter_row(row, prev, bpp, ftype):
    out = bytearray(len(row))
    for i in range(len(row)):
        x = row[i]
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i] if prev is not None else 0
        c = prev[i - bpp] if (prev is not None and i >= bpp) else 0
        if ftype == 0:
            pred = 0
        elif ftype == 1:
            pred = a
        elif ftype == 2:
            pred = b
        elif ftype == 3:
            pred = (a + b) // 2
        else:
            pred = paeth(a, b, c)
        out[i] = (x - pred) & 0xff
    return bytes([ftype]) + bytes(out)

def write_png(path, w, h, ctype, raw_rows, plte=None, trns=None, interlace=0):
    ihdr = struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, interlace)
    png = bytearray(b"\x89PNG\r\n\x1a\n")
    png += chunk(b"IHDR", ihdr)
    if plte is not None:
        png += chunk(b"PLTE", plte)
    if trns is not None:
        png += chunk(b"tRNS", trns)
    png += chunk(b"IDAT", zlib.compress(b"".join(raw_rows), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(bytes(png))

def filtered_rows(pixel_rows, bpp, ftypes):
    rows, prev = [], None
    for y, prow in enumerate(pixel_rows):
        rows.append(filter_row(prow, prev, bpp, ftypes(y)))
        prev = prow
    return rows

def main():
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    W, H = 1024, 768

    # 1. grayscale 8-bit
    px = [bytes(((x * 3 + y * 7 + ((x // 16) ^ (y // 16)) * 11) & 0xff
                 for x in range(W))) for y in range(H)]
    write_png(os.path.join(outdir, "gray8_1024x768.png"), W, H, 0,
              filtered_rows(px, 1, lambda y: y % 5))

    # 2. truecolor 8-bit
    px = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            row += bytes(((x * 255) // (W - 1), (y * 255) // (H - 1), (x ^ y) & 0xff))
        px.append(bytes(row))
    write_png(os.path.join(outdir, "rgb8_1024x768.png"), W, H, 2,
              filtered_rows(px, 3, lambda y: y % 5))

    # 3. indexed 8-bit + tRNS
    px = [bytes((((x // 8) ^ (y // 8)) & 0xff for x in range(W)))
          for y in range(H)]
    plte = b"".join(bytes(((i * 37) & 0xff, (i * 91) & 0xff, (i * 53) & 0xff))
                    for i in range(256))
    trns = bytes([0] + [255] * 255)  # palette index 0 is transparent
    write_png(os.path.join(outdir, "pal8_1024x768.png"), W, H, 3,
              filtered_rows(px, 1, lambda y: y % 5), plte=plte, trns=trns)

    # 4. truecolor + alpha 8-bit
    px = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            row += bytes(((x * 255) // (W - 1), (y * 255) // (H - 1),
                          (x ^ y) & 0xff, (x + y) & 0xff))
        px.append(bytes(row))
    write_png(os.path.join(outdir, "rgba8_1024x768.png"), W, H, 6,
              filtered_rows(px, 4, lambda y: y % 5))

    # 5. small truecolor, only None/Sub filters (decodable with ring size 1)
    W2 = H2 = 128
    px = []
    for y in range(H2):
        row = bytearray()
        for x in range(W2):
            row += bytes(((x * 2) & 0xff, (y * 2) & 0xff, (x + y) & 0xff))
        px.append(bytes(row))
    write_png(os.path.join(outdir, "subonly_128x128.png"), W2, H2, 2,
              filtered_rows(px, 3, lambda y: y % 2))

    # 6. interlaced (Adam7 flag set) - the decoder rejects this at open()
    px = [bytes(((x + y) & 0xff for x in range(W2))) for y in range(H2)]
    write_png(os.path.join(outdir, "interlaced_128x128.png"), W2, H2, 0,
              filtered_rows(px, 1, lambda y: 0), interlace=1)

    for f in sorted(os.listdir(outdir)):
        if f.endswith(".png"):
            print("%8d  %s" % (os.path.getsize(os.path.join(outdir, f)), f))

if __name__ == "__main__":
    main()
