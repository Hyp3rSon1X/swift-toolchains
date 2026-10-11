#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0005 (swiftlang/swift#92312): typed-throws closure passed to a generic untyped-throws parameter (Task {}).
# Builds each case with the stock compiler (informational: expected to crash or corrupt the error), swaps in the
# patched swift-frontend and rebuilds (must pass). STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
W=/tmp/sc0005; rm -rf "$W"; mkdir -p "$W"
UF="-swift-version 6 -enable-upcoming-feature NonisolatedNonsendingByDefault"

case_canary() { # -O | -Onone
  local d="$W/canary$1"; mkdir -p "$d"; local out
  timeout 300 swiftc $1 -parse-as-library $UF "$here/canary.swift" -o "$d/main" 2>&1 | tail -3
  [ "${PIPESTATUS[0]}" = 0 ] || return 1
  out=$(timeout 120 "$d/main" 2>&1); echo "run: $(echo $out | tail -c 200)"
  [ "$out" = "typed-throws canary: control and 2000 typed Task results passed" ]
}
case_values() {
  local d="$W/values$1"; mkdir -p "$d"; local out
  timeout 300 swiftc $1 -parse-as-library $UF "$here/values.swift" -o "$d/main" 2>&1 | tail -3
  [ "${PIPESTATUS[0]}" = 0 ] || return 1
  out=$(timeout 120 "$d/main" 2>&1); echo "run: $(echo $out)"
  [ "$out" = $'enum: c\nstruct: P(x: 42, y: "hello")\ngeneric: P(x: 7, y: "g")' ]
}

CASES=(
  "canary-O:case_canary -O"
  "canary-Onone:case_canary -Onone"
  "values-O:case_values -O"
  "values-Onone:case_values -Onone"
)

run_all() { # <label>; sets FAILS
  FAILS=0
  local c label
  for c in "${CASES[@]}"; do
    label="${c%%:*}"
    local log; log=$(${c#*:} 2>&1); local ok=$?
    echo "$1 $label: $([ $ok = 0 ] && echo pass || echo FAIL) | $(echo "$log" | tail -2 | tr '\n' ' ')"
    [ $ok = 0 ] || FAILS=$((FAILS+1))
  done
}

run_all stock; stock_bad=$FAILS
[ "$stock_bad" -gt 0 ] || echo "::warning::stock compiler passed every SC-0005 case on this runner; the test proves less here"

if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_failures=$stock_bad/${#CASES[@]} patched_failures=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
cp "$FE" /tmp/swift-frontend.stock
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

run_all patched; patched_bad=$FAILS
echo "SUMMARY stock_failures=$stock_bad/${#CASES[@]} patched_failures=$patched_bad"
[ "$patched_bad" = 0 ]
