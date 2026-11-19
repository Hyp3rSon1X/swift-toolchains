#!/bin/bash
# Builds libmacro_host.dylib against the WAMR 2.4.5 static library from build/swifthost/tools.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
T=${WAMR_TOOLS:-/Volumes/MEA-SSD/Hyp3rSuite/SwiftCollection/build/swifthost/tools}
OUT=${1:-/Volumes/MEA-SSD/Hyp3rSuite/SwiftCollection/build/swifthost-macros/dist-test}
mkdir -p "$OUT"
clang -O2 -dynamiclib -o "$OUT/libmacro_host.dylib" "$HERE/macro_host.c" \
  -I"$T/wamr-src/wasm-micro-runtime-WAMR-2.4.5/core/iwasm/include" \
  "$T/wamr-build/libiwasm.a" -lpthread -lm
