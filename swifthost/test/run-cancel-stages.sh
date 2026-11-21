#!/bin/bash
# Runs test/cancel-stages.c against one slice: swifthost_request_cancel() in SIL generation, the SIL optimizer, the
# SIL lowering passes, IR generation and LLVM optimization.
#
#   run-cancel-stages.sh macosx|iphonesimulator <lib> <sdk-target-dir> <work-dir>
#
# <lib> is a slice's prelinked SwiftHostCompiler.o (or a libSwiftHostCompiler.a that keeps its local symbols: the
# test finds the stages by the addresses of compiler functions in its own binary). <sdk-target-dir> is
# .../swift-6.4.2-RELEASE_wasm.artifactbundle/swift-6.4.2-RELEASE_wasm/wasm32-unknown-wasip1. The simulator run
# uses a temporary device or the shared one of a test session (sim-device.sh); stopping the script (HUP, INT, TERM)
# stops the test.
set -euo pipefail
SLICE=$1 LIB=$(cd "$(dirname "$2")" && pwd)/$(basename "$2") SDK=$3 WORK=$4
HERE=$(cd "$(dirname "$0")" && pwd)
INC=$(cd "$HERE/../include" && pwd)
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
case $SLICE in
  macosx)          TRIPLE=arm64-apple-macosx27.0;;
  iphonesimulator) TRIPLE=arm64-apple-ios27.0-simulator;;
  *) echo "slice must be macosx or iphonesimulator" >&2; exit 2;;
esac
SYSROOT=$(xcrun --sdk "$SLICE" --show-sdk-path)
TOOLCHAIN=$(dirname "$(dirname "$(xcrun -f swiftc)")")
xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -I"$INC" -O1 -Wall -Wextra -c "$HERE/cancel-stages.c" \
  -o "$WORK/cancel-stages.o"
xcrun clang++ -target $TRIPLE -isysroot "$SYSROOT" "$WORK/cancel-stages.o" "$LIB" \
  -L"$SYSROOT/usr/lib/swift" -L"$TOOLCHAIN/lib/swift/$SLICE" -dead_strip -o "$WORK/cancel-stages"

# "<stage> <lo> <hi>": the unslid address range of each function whose frame on the stack marks a stage (from the
# function's address to the next symbol's).
nm -n "$WORK/cancel-stages" | awk '
  $1 ~ /^[0-9a-f]+$/ && NF >= 3 {
    if (stage != "" && $1 != lo) { print stage, lo, $1; stage = "" }
    if (stage != "") next
    if ($3 ~ /18performASTLowering|18ASTLoweringRequest8evaluate/) stage = "SILGen"
    else if ($3 ~ /14SILPassManager7executeEv/) stage = "SILPipe"
    else if ($3 ~ /20runSILLoweringPasses/) stage = "SILLower"
    else if ($3 ~ /^__ZNK5swift12IRGenRequest8evaluate/) stage = "IRGen"
    else if ($3 ~ /^__ZN5swift24performLLVMOptimizations/) stage = "LLVMOpt"
    lo = $1
  }' > "$WORK/stages.txt"
for stage in SILGen SILPipe SILLower IRGen LLVMOpt; do
  grep -q "^$stage " "$WORK/stages.txt" || { echo "no symbol for stage $stage in the test binary" >&2; exit 2; }
done

if [[ $SLICE == macosx ]]; then
  exec "$WORK/cancel-stages" "$SDK" "$WORK" "$WORK/stages.txt"
fi
. "$HERE/sim-device.sh"
sim_device_create swifthost-cancel-stages || exit 2
proc_run xcrun simctl spawn "$SIM_UDID" "$WORK/cancel-stages" "$SDK" "$WORK" "$WORK/stages.txt"
