# Stride-output tests

Verification for the GPU-friendly stride output added in `src/PNGdec.h`
(`PNGStrideConfig` / `PNGStride_draw` / `PNGStride_*` helpers). Nothing here
uses third-party libraries or the network: PNG fixtures are synthesized with
the Python standard library (`zlib`, `struct`, `binascii`) and the harness
links only the in-repo decoder objects plus libc/libpthread.

## Run

```sh
./run_tests.sh
```

One command rebuilds the library objects with the repo's own flags and runs
everything:

1. `make_pngs.py`  - generates four tiny fixtures in a build dir:
   - `nonaligned_rgb.png`  7x5 RGB    (21 B/row, odd width)
   - `aligned_rgb.png`     8x4 RGB    (24 B/row)
   - `alpha_rgba.png`      7x5 RGBA   (real alpha channel)
   - `palette_idx.png`     7x5 indexed with `tRNS` palette alpha
2. `stride_test.cpp` - per image: output layout, pixel-for-pixel equality
   against both the compact decode and a hand-written per-line copy,
   padding fill / no-fill, vertical flip, stride-too-small rejection,
   timing of the three decode paths and buffer-size delta.
3. `c_smoke.c` - the same API compiled and exercised as plain C.
4. The repo's existing `linux/main.cpp` demo on a repo-shipped PNG
   (regression guard).

All build output stays under `tests/build/`; no build scripts or installed
files are touched.
