# PNGdec decode-provenance (debug tracing) tests

Optional per-pixel **decode provenance** for PNGdec, plus the tests and tiny
synthetic samples that exercise it. Everything here uses only the repository's
own sources and the system toolchain / Python standard library — no third-party
packages and no network access.

## What is recorded

For every output pixel the trace stores where it came from:

| field          | meaning |
|----------------|---------|
| `ucFilter`     | PNG filter of the scanline that produced it: 0=None … 4=Paeth |
| `ucPass`       | Adam7 pass 1..7; always 0 for non-interlaced images |
| `ucTransparent`| 1 if the pixel is transparent (alpha 0 / palette tRNS 0 / tRNS color), else 0 |
| `usSrcRow`     | scanline index inside its Adam7 pass (the raster row for non-interlaced) |

## Switches

1. **Compile-time master switch** — `PNG_PROVENANCE`, default `0`.
   * `0` (default): every trace call is a no-op macro and all trace state is
     compiled out. The decoder is bit-for-bit identical in output and cost.
   * `1`: trace code and the `PNG_TraceBegin/End` debug API are compiled in.
2. **Runtime granularity** — only meaningful when the feature is compiled in:
   * `PNG_TRACE_OFF` (default), `PNG_TRACE_LINE` (one record per scanline),
     `PNG_TRACE_PIXEL` (one record per output pixel).

No public header signature changes: the debug API lives in `png.inl` as
`static` functions, reachable from a single-translation-unit build (the same
include style the bundled `MacOS/PNG_Test` already uses).

## Layout

* `gen_samples.py` — generates the small synthetic PNGs plus decode oracles
  (`.raw` expected framebuffer, `.tr` expected transparency flags in decoder
  emission order, `.prov` expected per-scanline filter/pass/row). Run with
  `python3 -I`.
* `prov_test.cpp` — single-TU ASan/UBSan harness. Built twice by `run.sh`:
  with `-DPNG_PROVENANCE=1` and without it.
* `verify_ppm.py` — independent stdlib verifier for the PPM index maps.
* `ubsan_suppressions.txt` — silences only pre-existing alignment reports in
  the bundled zlib `inflate.c`.
* `samples/` — generated inputs (kept separate from the scripts).
* `out/` — run logs, `*_stats.txt` tables, `*_provenance.ppm` (filter color,
  white = transparent) and `*_passes.ppm` (one color per Adam7 pass).
* `run.sh` — one command regenerates samples, builds both variants, runs every
  check, and runs the repository's own existing test suite.

## The four required image categories

| sample             | category                    | what the trace proves |
|--------------------|-----------------------------|-----------------------|
| `filters_gray8`    | all 5 filters               | every filter type appears; per-pixel filter id matches the encoder |
| `interlace_rgb`    | Adam7 interlaced (truecolor)| pass ids 1..7 with the standard pixel counts; scatter geometry |
| `interlace_pal2`   | interlaced + 2-bit palette  | sub-byte unpack/re-pack across passes; palette tRNS transparency |
| `palette8_trns`    | palette + alpha (tRNS)      | per-pixel transparent flags from palette alpha |
| `alpha_rgba`       | truecolor + alpha channel   | per-pixel transparent flags from the alpha byte |

## Cost (measured by the harness on this machine, 64x48 RGBA)

* Extra memory is caller-supplied only: 6 bytes/pixel in pixel mode
  (8 bytes/line in line mode); no decoder struct grew.
* Runtime: line-granularity tracing is within noise of a plain decode;
  pixel-granularity tracing is ~15% slower here (see `out/run_on.log`).
* Compiled-out build: no trace state is linked; output is byte-identical to the
  original library (also verified directly against the pre-change `png.inl`).
