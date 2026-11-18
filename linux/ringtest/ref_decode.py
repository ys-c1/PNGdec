#!/usr/bin/env python3
"""Independent reference PNG decoder (stdlib only) for cross-checking.

Usage: ref_decode.py <dir-with-pngs>

Decodes every non-interlaced fixture to raw pixels and writes <name>.raw
next to it, plus prints a crc32 of the raw pixels. This is the ground
truth that the C test harness compares the library output against.
"""
import zlib, struct, os, sys, binascii

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c

def decode(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "bad signature"
    pos, idat, hdr = 8, b"", None
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", payload)
        elif typ == b"IDAT":
            idat += payload
        pos += 12 + ln
    w, h, bitdepth, ctype, comp, filt, inter = hdr
    assert bitdepth == 8 and inter == 0, "reference only handles 8-bit non-interlaced"
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    pitch = w * ch
    raw = zlib.decompress(idat)
    out = bytearray()
    prev = bytearray(pitch)
    pos = 0
    for y in range(h):
        f = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + pitch]); pos += pitch
        if f == 1:    # Sub
            for i in range(ch, pitch):
                line[i] = (line[i] + line[i - ch]) & 0xff
        elif f == 2:  # Up
            for i in range(pitch):
                line[i] = (line[i] + prev[i]) & 0xff
        elif f == 3:  # Average
            for i in range(pitch):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + (a + prev[i]) // 2) & 0xff
        elif f == 4:  # Paeth
            for i in range(pitch):
                a = line[i - ch] if i >= ch else 0
                c = prev[i - ch] if i >= ch else 0
                line[i] = (line[i] + paeth(a, prev[i], c)) & 0xff
        else:
            assert f == 0, "unknown filter %d" % f
        out += line
        prev = line
    return bytes(out)

def main():
    d = sys.argv[1]
    for f in sorted(os.listdir(d)):
        if not f.endswith(".png") or "interlaced" in f:
            continue
        raw = decode(os.path.join(d, f))
        rawname = os.path.join(d, f[:-4] + ".raw")
        with open(rawname, "wb") as fh:
            fh.write(raw)
        print("%s  raw=%d bytes  crc32=%08x" %
              (f, len(raw), binascii.crc32(raw) & 0xffffffff))

if __name__ == "__main__":
    main()
