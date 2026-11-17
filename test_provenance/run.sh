#!/usr/bin/env bash
#
# One-shot build + test for the PNGdec debug-provenance feature.
# Uses only the repository's own sources and a system C/C++ toolchain.
# Does not touch any existing Makefile or public header.
#
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SAMPLES="$HERE/samples"
OUT="$HERE/out"
BUILD="$HERE/build"
mkdir -p "$SAMPLES" "$OUT" "$BUILD"

CFLAGS="-D__LINUX__ -Wall -Wextra -O2 -g -fsanitize=address,undefined -Wno-implicit-fallthrough -Wno-register -Wno-unused-parameter"
CXX=${CXX:-g++}

echo "=== 1. generate tiny synthetic samples (stdlib python, -I) ==="
python3 -I "$HERE/gen_samples.py" "$SAMPLES" || exit 1

echo
echo "=== 2. build provenance harness (PNG_PROVENANCE=1) ==="
$CXX $CFLAGS -DPNG_PROVENANCE=1 "$HERE/prov_test.cpp" -o "$BUILD/prov_test_on" || exit 1

echo
echo "=== 3. build harness with provenance compiled out (default) ==="
$CXX $CFLAGS "$HERE/prov_test.cpp" -o "$BUILD/prov_test_off" || exit 1

echo
echo "=== 4a. run: provenance ENABLED build ==="
UBSAN_OPTIONS="suppressions=$HERE/ubsan_suppressions.txt:print_stacktrace=0" \
ASAN_OPTIONS="detect_leaks=0" \
"$BUILD/prov_test_on" "$SAMPLES" "$OUT" 2>&1 | tee "$OUT/run_on.log"
RC_ON=${PIPESTATUS[0]}

echo
echo "=== 4b. run: provenance DISABLED build (pixel-identical check) ==="
UBSAN_OPTIONS="suppressions=$HERE/ubsan_suppressions.txt:print_stacktrace=0" \
ASAN_OPTIONS="detect_leaks=0" \
"$BUILD/prov_test_off" "$SAMPLES" "$OUT" 2>&1 | tee "$OUT/run_off.log"
RC_OFF=${PIPESTATUS[0]}

echo
echo "=== 4c. independent PPM index-map verification (stdlib python, -I) ==="
python3 -I "$HERE/verify_ppm.py" "$SAMPLES" "$OUT"
RC_PPM=$?

echo
echo "=== 5. run the repository's own existing test suite (ASan/UBSan) ==="
SUITE_DIR="$ROOT/MacOS/PNG_Test/PNG_Test"
make -C "$SUITE_DIR" clean >/dev/null 2>&1
make -C "$SUITE_DIR" 2>"$OUT/suite_build_warnings.log"
"$SUITE_DIR/png_test" 2>&1 | tee "$OUT/existing_suite.log" | tail -1
RC_SUITE=${PIPESTATUS[0]}
# the suite reports via its summary line; treat any nonzero "N failed" as failure
FAILS=$(grep -Eo "[0-9]+ failed" "$OUT/existing_suite.log" | tail -1 | awk '{print $1}')
if [ "${FAILS:-1}" = "0" ]; then
    RC_SUITE=0
else
    RC_SUITE=1
fi
make -C "$SUITE_DIR" clean >/dev/null 2>&1 # don't leave build artifacts in the source tree

echo
echo "=== binary size / linked trace code comparison ==="
size "$BUILD/prov_test_on" "$BUILD/prov_test_off" 2>/dev/null | tee "$OUT/binary_size.txt"

echo
echo "=== artifacts ==="
ls -1 "$OUT" | sed "s#^#  $OUT/#"

echo
if [ "$RC_ON" -eq 0 ] && [ "$RC_OFF" -eq 0 ] && [ "$RC_PPM" -eq 0 ] && [ "$RC_SUITE" -eq 0 ]; then
    echo "ALL GROUPS PASSED"
    exit 0
fi
echo "FAILURES: harness_on=$RC_ON harness_off=$RC_OFF ppm=$RC_PPM existing_suite=$RC_SUITE"
exit 1
