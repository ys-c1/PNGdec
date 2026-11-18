#!/usr/bin/env python3 -I
# Test-only tool: generates 300 small PNGs covering every pixel format the
# decoder supports (gray 1/2/4/8, rgb8, indexed 1/2/4/8 +/- tRNS, gray+alpha8,
# rgba8), across awkward widths, all 5 filter types, and split IDAT chunks.
# Uses only the Python standard library. Not part of the library.
import os, struct, zlib, random, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "pngs"
os.makedirs(OUT, exist_ok=True)

def chunk(typ, data):
    c = struct.pack(">I", len(data)) + typ + data
    return c + struct.pack(">I", zlib.crc32(typ + data) & 0xffffffff)

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc: return a
    if pb <= pc: return b
    return c

def apply_filter(ftype, raw, prev, bpp):
    out = bytearray(len(raw))
    for i, x in enumerate(raw):
        a = raw[i - bpp] if i >= bpp else 0
        b = prev[i] if prev else 0
        c = prev[i - bpp] if (prev and i >= bpp) else 0
        if ftype == 0: f = x
        elif ftype == 1: f = x - a
        elif ftype == 2: f = x - b
        elif ftype == 3: f = x - ((a + b) >> 1)
        else: f = x - paeth(a, b, c)
        out[i] = f & 0xff
    return bytes([ftype]) + bytes(out)

def pack_bits(indices, bpp):
    # pack pixel indices MSB-first for sub-byte formats
    out = bytearray()
    acc, nbits = 0, 0
    for v in indices:
        acc = (acc << bpp) | v
        nbits += bpp
        if nbits == 8:
            out.append(acc); acc, nbits = 0, 0
    if nbits:
        out.append(acc << (8 - nbits))
    return bytes(out)

def make_image(rng, w, h, ctype, bpp, filters):
    # returns list of raw (unfiltered) scanlines (bytes), plus filter bpp
    lines = []
    if ctype == 0:  # grayscale
        if bpp == 8:
            fbpp = 1
            for y in range(h):
                lines.append(bytes(rng.randrange(256) for _ in range(w)))
        else:
            fbpp = 1
            maxv = (1 << bpp) - 1
            for y in range(h):
                lines.append(pack_bits([rng.randrange(maxv + 1) for _ in range(w)], bpp))
    elif ctype == 2:  # truecolor 8
        fbpp = 3
        for y in range(h):
            lines.append(bytes(rng.randrange(256) for _ in range(w * 3)))
    elif ctype == 3:  # indexed
        fbpp = 1
        maxv = (1 << bpp) - 1
        for y in range(h):
            idx = [rng.randrange(maxv + 1) for _ in range(w)]
            lines.append(pack_bits(idx, bpp) if bpp < 8 else bytes(idx))
    elif ctype == 4:  # gray + alpha
        fbpp = 2
        alphas = [0, 255, 1, 128, 254, 77, 200, 3]
        for y in range(h):
            row = bytearray()
            for x in range(w):
                row.append(rng.randrange(256))
                row.append(alphas[(x + y) % len(alphas)])
            lines.append(bytes(row))
    elif ctype == 6:  # rgba
        fbpp = 4
        alphas = [0, 255, 1, 128, 254, 90, 180, 5]
        for y in range(h):
            row = bytearray()
            for x in range(w):
                row += bytes([rng.randrange(256), rng.randrange(256), rng.randrange(256)])
                row.append(alphas[(x * 3 + y) % len(alphas)])
            lines.append(bytes(row))
    return lines, fbpp

def build_png(path, w, h, ctype, bpp, filters, palette=None, trns=None, split_idat=False, seed=0):
    rng = random.Random(seed)
    lines, fbpp = make_image(rng, w, h, ctype, bpp, filters)
    raw = bytearray()
    prev = None
    for y, line in enumerate(lines):
        ft = filters[y % len(filters)]
        raw += apply_filter(ft, line, prev, fbpp)
        prev = line
    comp = zlib.compress(bytes(raw), 6)
    ihdr = struct.pack(">IIBBBBB", w, h, bpp, ctype, 0, 0, 0)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    if palette is not None:
        data += chunk(b"PLTE", palette)
    if trns is not None:
        data += chunk(b"tRNS", trns)
    if split_idat and len(comp) > 8:
        half = len(comp) // 2
        data += chunk(b"IDAT", comp[:half]) + chunk(b"IDAT", comp[half:])
    else:
        data += chunk(b"IDAT", comp)
    data += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)

# format matrix: (ctype, bpp, with_trns)
FORMATS = [
    (0, 1, False), (0, 2, False), (0, 4, False), (0, 8, False),
    (2, 8, False),
    (3, 1, False), (3, 1, True), (3, 2, False), (3, 2, True),
    (3, 4, False), (3, 4, True), (3, 8, False), (3, 8, True),
    (4, 8, False),
    (6, 8, False),
]
WIDTHS = [1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 48]
FILTERS = [[0], [1], [2], [3], [4], [0, 1, 2, 3, 4]]

N = 300
manifest = []
for i in range(N):
    ctype, bpp, use_trns = FORMATS[i % len(FORMATS)]
    w = WIDTHS[(i * 7 + 3) % len(WIDTHS)]
    h = 1 + (i * 5) % 24
    filters = FILTERS[i % len(FILTERS)]
    palette = trns = None
    if ctype == 3:
        rng = random.Random(1000 + i)
        ncol = 1 << bpp
        palette = bytes(v for _ in range(ncol) for v in
                        (rng.randrange(256), rng.randrange(256), rng.randrange(256)))
        if use_trns:
            # alpha values hitting 0, 255 and partial blends
            trns = bytes((0 if j % 3 == 0 else (255 if j % 3 == 1 else 37 + j * 60 % 200))
                         for j in range(ncol))
    split = (i % 7 == 0)
    name = "img_%03d_t%d_b%d%s_w%d_h%d.png" % (
        i, ctype, bpp, "_trns" if use_trns else "", w, h)
    build_png(os.path.join(OUT, name), w, h, ctype, bpp, filters,
              palette=palette, trns=trns, split_idat=split, seed=i)
    manifest.append(name)

with open(os.path.join(OUT, "manifest.txt"), "w") as f:
    f.write("\n".join(manifest) + "\n")
print("generated %d PNGs in %s" % (N, OUT))
