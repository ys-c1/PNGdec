#!/bin/sh
# Build the test harness against a given source dir.
# usage: build.sh <srcdir> <outbin>
set -e
SRC="$1"
OUT="$2"
CFLAGS="-D__LINUX__ -O2 -Wall"
TMP=$(mktemp -d)
for f in adler32 crc32 infback inffast inflate inftrees zutil; do
    gcc $CFLAGS -I"$SRC" -c "$SRC/$f.c" -o "$TMP/$f.o"
done
g++ $CFLAGS -I"$SRC" -c "$SRC/PNGdec.cpp" -o "$TMP/PNGdec.o"
g++ $CFLAGS -I"$SRC" -c "$(dirname "$0")/dump_decode.cpp" -o "$TMP/dump_decode.o"
g++ -O2 "$TMP"/*.o -o "$OUT"
rm -rf "$TMP"
