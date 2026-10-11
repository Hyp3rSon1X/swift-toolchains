#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0022: a typed-throws closure whose context type is a type parameter (stored in [() throws(E) -> T], passed through a generic id<T>) crashed the compiler in SILGen's prolog.
# Builds each case with the stock compiler (informational: expected to show the bug), swaps in the
# patched swift-frontend and rebuilds (must pass). STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
W=/tmp/sc0022; rm -rf "$W"; mkdir -p "$W"
# Builds <src> with <flags>, runs it, prints the output; succeeds only when it exits 0 with <expected> output.
case_run() { # <name> <src> <expected> <flags...>
  local name="$1" src="$2" expect="$3"; shift 3
  local d="$W/$name"; mkdir -p "$d"; local out rc
  timeout 300 swiftc "$@" "$here/$src" -o "$d/main" 2>&1 | tail -3
  [ "${PIPESTATUS[0]}" = 0 ] || return 1
  out=$(timeout 120 "$d/main" 2>&1); rc=$?; echo "run: exit $rc $(echo $out | tail -c 200)"
  [ "$rc" = 0 ] && [ "$out" = "$expect" ]
}
CASES=(
  "array-O:case_run aO arr63.swift \"SE(m: \\\"a\\\")\" -O -parse-as-library"
  "array-Onone:case_run aOnone arr63.swift \"SE(m: \\\"a\\\")\" -Onone -parse-as-library"
  "id-O:case_run iO idcase.swift \"1 id a\" -O -parse-as-library"
  "id-Onone:case_run iOnone idcase.swift \"1 id a\" -Onone -parse-as-library"
)

run_all() { # <label>; sets FAILS
  FAILS=0
  local c label
  for c in "${CASES[@]}"; do
    label="${c%%:*}"
    local log; log=$(eval "${c#*:}" 2>&1); local ok=$?
    echo "$1 $label: $([ $ok = 0 ] && echo pass || echo FAIL) | $(echo "$log" | tail -2 | tr '\n' ' ')"
    [ $ok = 0 ] || FAILS=$((FAILS+1))
  done
}

run_all stock; stock_bad=$FAILS
[ "$stock_bad" -gt 0 ] || echo "::warning::stock compiler passed every SC-0022 case on this runner; the test proves less here"

if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_failures=$stock_bad/${#CASES[@]} patched_failures=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
cp "$FE" /tmp/swift-frontend.stock
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

run_all patched; patched_bad=$FAILS
echo "SUMMARY stock_failures=$stock_bad/${#CASES[@]} patched_failures=$patched_bad"
[ "$patched_bad" = 0 ]
