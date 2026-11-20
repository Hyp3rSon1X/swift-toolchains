#!/bin/bash
# Links skd-proof.c against a macOS SwiftHostCompiler.o built with SOURCEKIT=1 and runs it.
#
#   run-proof.sh <SwiftHostCompiler.o or .a> <sdk-target-dir> <work-dir>
#
# <sdk-target-dir> is .../<bundle>/wasm32-unknown-wasip1-threads of a wasip1-threads Swift SDK.
# <work-dir> receives the binary and the module cache (<work-dir>/mc, kept between runs).
# SRC is the source root of build.sh (default ./src), for sourcekitd.h.
set -euo pipefail
LIB=$(cd "$(dirname "$1")" && pwd)/$(basename "$1") SDK=$(cd "$2" && pwd) WORK=$3
HERE=$(cd "$(dirname "$0")" && pwd)
SKD_INC=$(cd "${SRC:-src}" && pwd)/swift/tools/SourceKit/tools/sourcekitd/include
mkdir -p "$WORK/mc"
WORK=$(cd "$WORK" && pwd)
SYSROOT=$(xcrun --sdk macosx --show-sdk-path)
TOOLCHAIN=$(dirname "$(dirname "$(xcrun -f swiftc)")")
xcrun clang -target arm64-apple-macosx27.0 -isysroot "$SYSROOT" -I"$SKD_INC" -O1 -Wall -Wextra \
  -c "$HERE/skd-proof.c" -o "$WORK/skd-proof.o"
xcrun clang++ -target arm64-apple-macosx27.0 -isysroot "$SYSROOT" "$WORK/skd-proof.o" "$LIB" \
  -L"$SYSROOT/usr/lib/swift" -L"$TOOLCHAIN/lib/swift/macosx" -dead_strip -o "$WORK/skd-proof"
echo "skd-proof binary: $(stat -f %z "$WORK/skd-proof") bytes"
"$WORK/skd-proof" "$HERE" "$SDK" "$WORK/virtual" "$WORK/mc"
