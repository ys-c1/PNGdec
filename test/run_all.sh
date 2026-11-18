#!/bin/sh
# One-command regression check for the descriptor-table refactor.
# 1. extracts the pre-refactor sources from git HEAD into a temp dir
# 2. builds the dump/bench harness against both baseline and current src
# 3. decodes all 300 test PNGs under 8 runtime configs with both builds
#    and requires bit-identical RGB565 output
# 4. benchmarks both builds (interleaved) and reports the delta
# usage: run_all.sh [reps]
set -e
cd "$(dirname "$0")"
REPS=${1:-100}
BASE=$(mktemp -d)/src
mkdir -p "$BASE"
for f in png.inl PNGdec.cpp PNGdec.h PNGDisplay.h PNGDisplay.inl \
         adler32.c crc32.c crc32.h infback.c inffast.c inffast.h inffixed.h \
         inflate.c inflate.h inftrees.c inftrees.h zconf.h zlib.h zutil.c zutil.h; do
    git show "HEAD:src/$f" > "$BASE/$f"
done
[ -d pngs ] || python3 -I gen_pngs.py pngs
./build.sh "$BASE" /tmp/pngdec_dump_base
./build.sh ../src /tmp/pngdec_dump_new
rm -rf /tmp/pngdec_out_base /tmp/pngdec_out_new
mkdir -p /tmp/pngdec_out_base /tmp/pngdec_out_new
while read f; do
    /tmp/pngdec_dump_base dump "pngs/$f" "/tmp/pngdec_out_base/$f.raw"
    /tmp/pngdec_dump_new  dump "pngs/$f" "/tmp/pngdec_out_new/$f.raw"
done < pngs/manifest.txt
if diff -rq /tmp/pngdec_out_base /tmp/pngdec_out_new > /dev/null; then
    echo "PASS: 300 images x 8 configs bit-identical"
else
    echo "FAIL: output mismatch"; exit 1
fi
echo "benchmark (ns for $REPS reps x 300 images, interleaved):"
for i in 1 2 3 4 5; do
    /tmp/pngdec_dump_base bench pngs "$REPS"
    /tmp/pngdec_dump_new  bench pngs "$REPS"
done | paste - - | awk '{if($1<bm||NR==1)bm=$1; if($2<nm||NR==1)nm=$2; bs+=$1; ns+=$2} \
    END {printf "base min %d avg %.0f\nnew  min %d avg %.0f\ndelta min %+.2f%% avg %+.2f%%\n", \
    bm, bs/NR, nm, ns/NR, (nm-bm)*100.0/bm, (ns-bs)*100.0/bs}'
