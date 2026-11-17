#!/bin/sh
# Build and run the indexed-output tests, then the repository's existing suite.
#
# One shot, no network, no third-party dependencies: this only uses the
# compiler, make and the Python standard library already on the machine, plus
# the zlib/inflate sources that ship in ../src.
#
# Steps:
#   1. generate the tiny PNG samples
#   2. build the modified library + driver, run the index/RGB/error/timing tests
#   3. build a pristine baseline (git HEAD's src) and compare RGB565 output
#   4. build and run the repository's existing MacOS/PNG_Test suite
# Any failing step makes this script exit non-zero.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BUILD="$HERE/build"
SRC="$ROOT/src"

CXX=${CXX:-g++}
CC=${CC:-gcc}
CFLAGS="-D__LINUX__ -O2 -g -Wall"
OBJS="adler32 crc32 infback inffast inflate inftrees zutil"

rm -rf "$BUILD"
mkdir -p "$BUILD/baseline/src"

echo "== 1/4 generating samples"
python3 -I "$HERE/gen_samples.py"

echo "== 2/4 building modified tree"
compile_lib() { # $1 = src dir, $2 = out dir
    for o in $OBJS; do
        $CC $CFLAGS -c "$1/$o.c" -o "$2/$o.o"
    done
    $CXX $CFLAGS -c "$1/PNGdec.cpp" -o "$2/PNGdec.o"
}
compile_lib "$SRC" "$BUILD"
$CXX $CFLAGS "$HERE/driver.cpp" "$BUILD"/*.o -o "$BUILD/driver"

echo "== 3/4 building pristine baseline from git HEAD and comparing RGB output"
git -C "$ROOT" archive HEAD src | tar -x -C "$BUILD/baseline"
compile_lib "$BUILD/baseline/src" "$BUILD/baseline"
$CXX $CFLAGS -I "$BUILD/baseline/src" "$HERE/dump_rgb565.cpp" "$BUILD/baseline"/*.o \
    -o "$BUILD/dump_rgb565"
"$BUILD/dump_rgb565" "$HERE/samples" > "$BUILD/baseline_rgb.txt"

echo
"$BUILD/driver" "$HERE/samples" "$BUILD/baseline_rgb.txt"
echo

echo "== 3b/4 rebuilding with AddressSanitizer and re-running the driver"
mkdir -p "$BUILD/asan"
ASAN="-D__LINUX__ -O1 -g -Wall -fsanitize=address,undefined -fno-sanitize=alignment"
for o in $OBJS; do
    $CC $ASAN -c "$SRC/$o.c" -o "$BUILD/asan/$o.o"
done
$CXX $ASAN -c "$SRC/PNGdec.cpp" -o "$BUILD/asan/PNGdec.o"
$CXX $ASAN "$HERE/driver.cpp" "$BUILD/asan"/*.o -o "$BUILD/asan/driver"
"$BUILD/asan/driver" "$HERE/samples" "$BUILD/baseline_rgb.txt"
echo

echo "== 4/4 existing repository test suite (MacOS/PNG_Test)"
make -C "$ROOT/MacOS/PNG_Test/PNG_Test" clean >/dev/null
make -C "$ROOT/MacOS/PNG_Test/PNG_Test" >/dev/null
"$ROOT/MacOS/PNG_Test/PNG_Test/png_test"
