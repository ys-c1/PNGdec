# Decode-time linearization tests

Self-contained, one-shot tests for the optional sRGB/gAMA → linear-light
decoding added in `src/png.inl`. No third-party code and no network access:
the only dependencies are the repo's own sources, the C++ standard library
and Python's standard library.

## Run

```sh
./run_tests.sh
```

The script does three things and exits non-zero on any failure:

1. builds the repo's existing Linux demo straight from `../src` and decodes
   the two PNG files shipped in the repo (`perf_small.png`,
   `test_images/homer_2bpp.png`) — the current test baseline
2. regenerates the tiny fixtures under `samples/` with `make_samples.py`
   (stdlib `zlib` only, every file is ~110–160 bytes)
3. builds and runs `linear_test.cpp`

## What is covered

- default decode path byte-identical for every fixture (feature off)
- untagged file (no sRGB, no gAMA): exact passthrough
- malformed `gAMA` (value 0): ignored, no divide-by-zero
- sRGB + gAMA conflict: sRGB wins (PNG spec preference order)
- forward LUT error vs exact transfer math, 8-bit and 16-bit, across the
  sRGB / gAMA / gray+alpha / indexed classes
- round-trip behaviour (8-bit quantization is monotonic; 16-bit is exact)
- optional linear-space alpha premultiply (RGBA and indexed palette)
- parameter guards (16-bit rejected for callback/indexed paths)
- per-class timing and memory accounting

The reference transfer functions in `linear_test.cpp` use `<cmath>`; the
library itself does not link libm — its `lin_log/lin_exp/lin_pow` are
self-contained and were checked against libm to ~1e-16.
