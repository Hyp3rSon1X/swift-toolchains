#!/bin/bash
# Builds skd-cancel.c against a SwiftHostCompiler.o (or .a) built with SOURCEKIT=1 and runs it: cancelled
# diagnostics and completion requests must leave nothing behind that the next request reuses.
#
#   run-cancel.sh <SwiftHostCompiler.o or .a> <work-dir> <sdk-target-dir> [skd-cancel options...]
#
# <sdk-target-dir> is the wasm32-unknown-wasip1-threads directory of the Swift SDK. SLICE=iphonesimulator builds
# for and runs in the simulator (slice.sh). SRC is the source root of build.sh (default ./src), for sourcekitd.h.
set -euo pipefail
LIB=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
mkdir -p "$2"
WORK=$(cd "$2" && pwd)
SDK=$3
shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/slice.sh"
SKD_INC=$(cd "${SRC:-src}" && pwd)/swift/tools/SourceKit/tools/sourcekitd/include
xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -I"$SKD_INC" -fblocks -O1 -Wall -Wextra \
  -c "$HERE/skd-cancel.c" -o "$WORK/skd-cancel.o"
xcrun clang++ -target $TRIPLE -isysroot "$SYSROOT" "$WORK/skd-cancel.o" "$LIB" $SLICE_LINK_EXTRA \
  -L"$SYSROOT/usr/lib/swift" -L"$TOOLCHAIN_LIB" -dead_strip -o "$WORK/skd-cancel"
mkdir -p "$WORK/mc"
slice_run "$WORK/skd-cancel" --sdk "$SDK" --mc "$WORK/mc" --workdir "$WORK" "$@"
