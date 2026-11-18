# PNG header cache — benchmark and verification

Self-contained test/benchmark harness for the parsed-header cache added to
`src/png.inl`. No third-party code, no network, no build-script changes: the
library sources are included directly, exactly like the repo's own
`MacOS/PNG_Test` harness.

## Files

- `gen_samples.py` — five tiny valid PNGs (~2.6 KB total), Python standard
  library only. Run with `python3 -I`.
- `cache_bench.cpp` — the four required scenarios, an all-miss baseline,
  invalid-parameter checks, pixel-exact and palette correctness checks, an
  open-only header microbenchmark and a pthread shared-cache test.
- `legacy_shim.h` — no-op stand-ins for the cache API when the benchmark is
  compiled against the pre-change library pulled from git HEAD.
- `run.sh` — one-shot driver. Any failure aborts with a non-zero exit code.

## What the driver builds and runs

1. samples
2. ASan + UBSan build → all scenarios and assertions
3. ThreadSanitizer build → multi-threaded shared-cache race check
4. plain `-O2` build → stable timing numbers
5. a second `-O2` build with `PNG_CACHE_FULL_FINGERPRINT` (whole-file hash)
6. a legacy build against the original `png.inl` from git HEAD → true
   old-vs-new A/B timing using identical loops
7. the repository's existing `MacOS/PNG_Test` suite. Its binary always exits
   0, so the script parses the "N failed" summary; the upstream wall-clock
   "Check CRC timing" case is flaky under scheduling noise (also observed on
   unmodified HEAD), so timing-only failures retry up to 5 times while any
   functional failure aborts immediately.

## Cache semantics

- Cached fields: width, height, bit depth, pixel type, pitch, interlace flag,
  transparent color/value and the PLTE/tRNS palette bytes.
- Fingerprint (default): FNV-1a over the 33-byte signature+IHDR prefix, the
  PLTE and tRNS chunk contents, and the total buffer length. Ancillary chunks
  that no cached field depends on and the IDAT pixel stream are not hashed.
  `PNG_CACHE_FULL_FINGERPRINT` switches to a whole-buffer hash where any byte
  edit forces a miss.
- The table is one statically allocated BSS array (no malloc); its exact size
  is printed via `nm -S`. Entries are immutable while published and guarded by
  a pthread mutex on POSIX; on single-threaded targets the locks are no-ops.
- `PNG_cache_configure(entries, policy, threads)` accepts entries 1..16,
  policy `PNG_CACHE_LRU`/`PNG_CACHE_FIFO`, threads 1..16. Capacity 0 (or any
  invalid argument) returns `PNG_INVALID_PARAMETER`; the cache can never be
  disabled by configuring size 0.

## Reported metrics

parses (full header parses), hits/misses and hit rate, wall-clock time per
frame, open-only header time, evictions, occupied/capacity entries and peak
RSS.
