#!/bin/sh
# Build and run the PNGdec row-view tests in one shot.
# Uses the compiler flags of the repo's existing MacOS/PNG_Test suite.
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
$CXX -Wall -Wextra -O2 -g -fsanitize=address,undefined rowview_test.cpp -o rowview_test
./rowview_test
