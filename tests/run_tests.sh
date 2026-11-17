#!/usr/bin/env bash
#
# run_tests.sh - one-shot test runner.
#   1. rebuilds the library objects with the repo's own flags (no install)
#   2. regenerates the four small PNG fixtures with the stdlib-only script
#   3. builds and runs the stride feature tests (C++ and C)
#   4. builds and runs the repo's existing Linux demo on the repo's own PNG
# No network access and no third-party dependencies are used.
set -u
cd "$(dirname "$0")/.."
ROOT="$PWD"
WORK="tests/build"
mkdir -p "$WORK"
CFLAGS="-D__LINUX__ -ggdb -Wall -Wextra -O2 -I$ROOT/src"

echo "== [1/5] rebuild library objects =="
for src in PNGdec.cpp adler32.c crc32.c infback.c inffast.c inflate.c \
           inftrees.c zutil.c; do
    out="$WORK/${src%.*}.o"
    case "$src" in
        *.cpp) g++ $CFLAGS -c "$ROOT/src/$src" -o "$out" || exit 1 ;;
        *.c)   gcc $CFLAGS -c "$ROOT/src/$src" -o "$out" || exit 1 ;;
    esac
done

echo "== [2/5] generate fixture PNGs =="
python3 -I tests/make_pngs.py "$WORK" || exit 1

echo "== [3/5] C++ stride tests =="
g++ $CFLAGS tests/stride_test.cpp "$WORK"/*.o -o "$WORK/stride_test" \
    -lpthread || exit 1
"$WORK/stride_test" "$WORK" || exit 1

echo "== [4/5] C API smoke test =="
gcc $CFLAGS tests/c_smoke.c "$WORK"/adler32.o "$WORK"/crc32.o \
    "$WORK"/infback.o "$WORK"/inffast.o "$WORK"/inflate.o \
    "$WORK"/inftrees.o "$WORK"/zutil.o -o "$WORK/c_smoke" -lpthread \
    || exit 1
"$WORK/c_smoke" || exit 1

echo "== [5/5] existing repo demo (regression) =="
g++ $CFLAGS "$ROOT/linux/main.cpp" "$WORK"/*.o -o "$WORK/png_demo" \
    -lpthread 2>/dev/null || exit 1
"$WORK/png_demo" "$ROOT/test_images/homer_2bpp.png" "$WORK/homer.bmp" || exit 1
ls -l "$WORK/homer.bmp"

echo
echo "ALL TESTS PASSED"
