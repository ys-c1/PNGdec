#!/usr/bin/env python3
"""
Independent verifier for the provenance PPM index maps.
Pure standard library; run with: python3 -I verify_ppm.py <sample_dir> <out_dir>

Cross-checks, pixel by pixel:
  *_provenance.ppm  filter color (and white=transparent) against *.prov/*.tr
  *_passes.ppm      Adam7 pass color against the standard Adam7 geometry
"""
import os
import sys

A7_START = [(0, 0), (4, 0), (0, 4), (2, 0), (0, 2), (1, 0), (0, 1)]
A7_STEP = [(8, 8), (8, 8), (4, 8), (4, 4), (2, 4), (2, 2), (1, 2)]

# must match prov_test.cpp
FILTER_COL = [(80, 80, 80), (220, 60, 60), (60, 180, 60), (60, 90, 230),
              (220, 170, 40)]
PASS_COL = [(40, 40, 40), (230, 50, 50), (230, 150, 50), (230, 230, 50),
            (50, 200, 50), (50, 200, 220), (70, 90, 230), (180, 70, 220)]
WHITE = (255, 255, 255)
BLACK = (0, 0, 0)


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", path
    i = 2
    values = []
    while len(values) < 3:
        # skip whitespace and comment lines
        while i < len(data):
            if data[i] in b" \t\r\n":
                i += 1
            elif data[i] == 35:  # '#' comment to end of line
                while i < len(data) and data[i] not in b"\r\n":
                    i += 1
            else:
                break
        j = i
        while j < len(data) and data[j] not in b" \t\r\n":
            j += 1
        values.append(int(data[i:j]))
        i = j
    # exactly one single whitespace byte separates maxval from raster data
    raster = i + 1
    w, h, maxval = values
    pix = data[raster:raster + w * h * 3]
    assert maxval == 255 and len(pix) == w * h * 3, path
    return w, h, pix


def emission(w, h, interlaced):
    if not interlaced:
        return [(x, y, 0, y) for y in range(h) for x in range(w)]
    out = []
    for p in range(7):
        sx, sy = A7_STEP[p]
        ox, oy = A7_START[p]
        for fy in range(oy, h, sy):
            for fx in range(ox, w, sx):
                out.append((fx, fy, p + 1, fy // sy))
    return out


def load_prov(path):
    rows = []
    with open(path) as f:
        for line in f:
            filt, passno, srcrow, n = map(int, line.split())
            rows.append((filt, passno, srcrow, n))
    return rows


def main():
    sdir, odir = sys.argv[1], sys.argv[2]
    failures = 0
    checks = 0
    for line in open(os.path.join(sdir, "manifest.txt")):
        name, w, h, ct, bpp, il = line.strip().split(",")
        w, h, il = int(w), int(h), int(il)
        prov = load_prov(os.path.join(sdir, name + ".prov"))
        tr = open(os.path.join(sdir, name + ".tr"), "rb").read()
        coords = emission(w, h, il)

        # expected filter per raster pixel, in emission order
        exp_filter = []
        for filt, passno, srcrow, n in prov:
            exp_filter.extend([filt] * n)
        assert len(exp_filter) == len(coords) == len(tr)

        ppm_path = os.path.join(odir, name + "_provenance.ppm")
        if os.path.exists(ppm_path):
            pw, ph, pix = read_ppm(ppm_path)
            assert (pw, ph) == (w, h)
            for i, (x, y, _, _) in enumerate(coords):
                got = tuple(pix[(y * w + x) * 3:(y * w + x) * 3 + 3])
                want = WHITE if tr[i] else FILTER_COL[exp_filter[i]]
                checks += 1
                if got != want:
                    failures += 1
                    print(f"FAIL {name} provenance ({x},{y}): got {got} "
                          f"want {want}")
                    if failures > 5:
                        break

        pass_path = os.path.join(odir, name + "_passes.ppm")
        if os.path.exists(pass_path):
            pw, ph, pix = read_ppm(pass_path)
            assert (pw, ph) == (w, h)
            for x, y, passno, _ in coords:
                got = tuple(pix[(y * w + x) * 3:(y * w + x) * 3 + 3])
                want = PASS_COL[passno]
                checks += 1
                if got != want:
                    failures += 1
                    print(f"FAIL {name} passes ({x},{y}): got {got} "
                          f"want {want}")
                    if failures > 5:
                        break
            # Adam7 covers every pixel: no black cells
            for y in range(h):
                for x in range(w):
                    got = tuple(pix[(y * w + x) * 3:(y * w + x) * 3 + 3])
                    checks += 1
                    if got == BLACK:
                        failures += 1
                        print(f"FAIL {name} uncovered black cell ({x},{y})")

    print(f"ppm verification: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
