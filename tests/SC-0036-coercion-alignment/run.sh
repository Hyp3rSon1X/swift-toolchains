#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0036: at -Onone, IRGen's store peepholes forwarded a coercion temporary (aligned for the native explosion's
# LLVM types, 8 bytes) as the address of a Swift value that needs 16 (SIMD4<Float>, UInt128 payloads), and the copy
# out of it used an aligned movaps: signal 11 on Linux x86_64 (swiftlang/swift#93129 is the same bug). aarch64 has
# no aligned-load trap, so it is informational there. Stock is informational (x86_64 -Onone is expected to crash);
# the patched frontend must build and run the repro with the expected output at -Onone and -O on every arch.
# STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
out=/tmp/sc0036; mkdir -p "$out"
EXPECT=$'3 clear(SIMD4<Float>(0.25, 0.5, 0.75, 1.0))\nclear clear(SIMD4<Float>(0.25, 0.5, 0.75, 1.0)) false\npreserve load true\ntrue false'

runs() { # <-Onone|-O> <label>; 0 when the repro builds, exits 0 and prints EXPECT
  local exe="$out/r$1"
  if ! (cd "$here" && timeout 300 swiftc "$1" repro.swift -o "$exe") >"$out/build$1.log" 2>&1; then
    echo "$2 $1: build FAIL $(grep -m1 -E 'error|Assertion' "$out/build$1.log" | cut -c1-120)"; return 1
  fi
  local o rc; o=$(timeout 120 "$exe" 2>&1); rc=$?
  if [ "$rc" = 0 ] && [ "$o" = "$EXPECT" ]; then echo "$2 $1: pass"; return 0; fi
  echo "$2 $1: FAIL exit $rc $(printf '%s' "$o" | grep -m1 -E 'crash|Fatal|fault|dereference' | cut -c1-100)"; return 1
}

arch=$(uname -m); stock_bad=0
for m in -Onone -O; do runs "$m" stock || stock_bad=$((stock_bad+1)); done
if [ "$arch" = x86_64 ] && [ "$stock_bad" = 0 ]; then
  echo "::warning::the stock compiler runs the SC-0036 repro on x86_64; check whether upstream fixed swiftlang/swift#93129"
fi
if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY arch=$arch stock_failures=$stock_bad/2 patched=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

patched_bad=0
for m in -Onone -O; do runs "$m" patched || patched_bad=$((patched_bad+1)); done
echo "SUMMARY arch=$arch stock_failures=$stock_bad/2 patched_failures=$patched_bad/2"
[ "$patched_bad" = 0 ]
