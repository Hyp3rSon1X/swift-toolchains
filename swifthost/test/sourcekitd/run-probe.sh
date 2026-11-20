#!/bin/bash
# Builds skd-probe (skd-probe.c + skd-probe-main.c) against a macOS SwiftHostCompiler.o built with SOURCEKIT=1
# and runs it; same link line as run-proof.sh.
#
#   run-probe.sh <SwiftHostCompiler.o or .a> <work-dir> [skd-probe options...]
#
# SLICE=iphonesimulator builds for and runs in the simulator (slice.sh).
# <work-dir> receives the binary (<work-dir>/skd-probe). With no options only the binary is built.
# SRC is the source root of build.sh (default ./src), for sourcekitd.h. Options: see skd-probe-main.c.
# The probe needs --yaml-dir (this directory is a good value), --sdk, --mc and the sources to probe.
set -euo pipefail
LIB=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
mkdir -p "$2"
WORK=$(cd "$2" && pwd)
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/slice.sh"
SKD_INC=$(cd "${SRC:-src}" && pwd)/swift/tools/SourceKit/tools/sourcekitd/include
for f in skd-probe skd-probe-main; do
  xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -I"$SKD_INC" -I"$HERE" -fblocks -O1 -Wall -Wextra \
    -c "$HERE/$f.c" -o "$WORK/$f.o"
done
xcrun clang++ -target $TRIPLE -isysroot "$SYSROOT" "$WORK/skd-probe.o" "$WORK/skd-probe-main.o" "$LIB" $SLICE_LINK_EXTRA \
  -L"$SYSROOT/usr/lib/swift" -L"$TOOLCHAIN_LIB" -dead_strip -o "$WORK/skd-probe"
# The concurrency probe (sourcekitd and swifthost_swift_frontend in one process) needs the swifthost header.
INC=$(cd "$HERE/../../include" && pwd)
xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -I"$SKD_INC" -I"$HERE" -I"$INC" -fblocks -O1 -Wall -Wextra \
  -c "$HERE/skd-concurrent.c" -o "$WORK/skd-concurrent.o"
xcrun clang++ -target $TRIPLE -isysroot "$SYSROOT" "$WORK/skd-probe.o" "$WORK/skd-concurrent.o" "$LIB" $SLICE_LINK_EXTRA \
  -L"$SYSROOT/usr/lib/swift" -L"$TOOLCHAIN_LIB" -dead_strip -o "$WORK/skd-concurrent"
echo "skd-probe binary: $(stat -f %z "$WORK/skd-probe") bytes" >&2
if [ $# -gt 0 ]; then slice_run "$WORK/skd-probe" "$@"; fi
