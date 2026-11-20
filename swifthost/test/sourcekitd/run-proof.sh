#!/bin/bash
# Links skd-proof.c against a macOS SwiftHostCompiler.o built with SOURCEKIT=1 and runs it.
#
#   run-proof.sh <SwiftHostCompiler.o or .a> <sdk-target-dir> <work-dir>
#
# <sdk-target-dir> is .../<bundle>/wasm32-unknown-wasip1-threads of a wasip1-threads Swift SDK.
# SLICE=iphonesimulator builds for and runs in the simulator (slice.sh).
# <work-dir> receives the binary and the module cache (<work-dir>/mc, kept between runs).
# SRC is the source root of build.sh (default ./src), for sourcekitd.h.
set -euo pipefail
LIB=$(cd "$(dirname "$1")" && pwd)/$(basename "$1") SDK=$(cd "$2" && pwd) WORK=$3
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/slice.sh"
SKD_INC=$(cd "${SRC:-src}" && pwd)/swift/tools/SourceKit/tools/sourcekitd/include
mkdir -p "$WORK/mc"
WORK=$(cd "$WORK" && pwd)
xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -I"$SKD_INC" -O1 -Wall -Wextra \
  -c "$HERE/skd-proof.c" -o "$WORK/skd-proof.o"
xcrun clang++ -target $TRIPLE -isysroot "$SYSROOT" "$WORK/skd-proof.o" "$LIB" $SLICE_LINK_EXTRA \
  -L"$SYSROOT/usr/lib/swift" -L"$TOOLCHAIN_LIB" -dead_strip -o "$WORK/skd-proof"
echo "skd-proof binary: $(stat -f %z "$WORK/skd-proof") bytes"
slice_run "$WORK/skd-proof" "$HERE" "$SDK" "$WORK/virtual" "$WORK/mc"
