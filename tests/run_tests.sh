#!/usr/bin/env bash
#
# tests/run_tests.sh - one-shot runner for the linearization feature.
#
# 1. builds the library's own Linux demo straight from src/ (no installs,
#    no network, no new third-party code) and runs it against the two PNG
#    files already shipped in the repo
# 2. generates tiny self-made fixtures with stdlib-only make_samples.py
# 3. builds and runs the linearization test harness
#
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"
SRC="$ROOT/src"
OUT=build
mkdir -p "$OUT" samples

SRCS_C="$SRC/adler32.c $SRC/crc32.c $SRC/infback.c $SRC/inffast.c \
$SRC/inflate.c $SRC/inftrees.c $SRC/zutil.c"
FLAGS="-D__LINUX__ -Wall -O2 -I$SRC"

# diagnostics that pre-date this change and come from vendored zlib /
# the existing demo; anything else counts as a new diagnostic.
filter_known() {
    grep -E "warning|error" || true
}
fail_on_new_diag() {
    if grep -E "warning|error" "$1" | \
       grep -v -E "implicit-fallthrough|register. storage class|warn_unused_result|unused parameter .iHasAlpha."; then
        echo "new code introduced compiler diagnostics"
        exit 1
    fi
}

echo "########## 1. Existing repo test path (linux demo, repo PNGs) ##########"
g++ $FLAGS "$ROOT/linux/main.cpp" "$SRC/PNGdec.cpp" $SRCS_C \
    -o "$OUT/png_demo" 2>"$OUT/build_demo.warn" || { cat "$OUT/build_demo.warn"; exit 1; }
cat "$OUT/build_demo.warn" | filter_known
fail_on_new_diag "$OUT/build_demo.warn"
"$OUT/png_demo" "$ROOT/perf_small.png" "$OUT/perf.bmp"
"$OUT/png_demo" "$ROOT/test_images/homer_2bpp.png" "$OUT/homer.bmp"

echo
echo "########## 2. Generate self-made fixtures (stdlib only) ##########"
python3 -I make_samples.py samples
ls -l samples

echo
echo "########## 3. Linearization test harness ##########"
g++ $FLAGS linear_test.cpp "$SRC/PNGdec.cpp" $SRCS_C -lm \
    -o "$OUT/linear_test" 2>"$OUT/build_test.warn" || { cat "$OUT/build_test.warn"; exit 1; }
fail_on_new_diag "$OUT/build_test.warn"
"$OUT/linear_test"
