#!/bin/bash
# Builds libmacro_host.dylib against the WAMR 2.4.5 static library from build/swifthost/tools.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
T=${WAMR_TOOLS:?set WAMR_TOOLS to the directory with wamr-src and wamr-build (WAMR 2.4.5)}
OUT=${1:-$PWD/build/dist-test}
mkdir -p "$OUT"
clang -O2 -dynamiclib -o "$OUT/libmacro_host.dylib" "$HERE/macro_host.c" \
  -I"$T/wamr-src/wasm-micro-runtime-WAMR-2.4.5/core/iwasm/include" \
  "$T/wamr-build/libiwasm.a" -lpthread -lm
