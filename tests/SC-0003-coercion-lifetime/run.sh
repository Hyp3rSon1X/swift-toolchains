#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0003: -O async functions taking an argument coerced through a stack temporary lost their spilled self/arguments (IRGen sized the temporary's lifetime markers in bits, swiftlang/swift#91312); reconcile_shape crashes or prints wrong values, upstream_probe prints zeros.
# Builds each case with the stock compiler (informational: expected to show the bug), swaps in the
# patched swift-frontend and rebuilds (must pass). STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
W=/tmp/sc0003; rm -rf "$W"; mkdir -p "$W"
# Builds <src> with <flags>, runs it, prints the output; succeeds only when it exits 0 with <expected> output.
case_run() { # <name> <src> <expected> <flags...>
  local name="$1" src="$2" expect="$3"; shift 3
  local d="$W/$name"; mkdir -p "$d"; local out rc
  timeout 300 swiftc "$@" "$here/$src" -o "$d/main" 2>&1 | tail -3
  [ "${PIPESTATUS[0]}" = 0 ] || return 1
  out=$(timeout 120 "$d/main" 2>&1); rc=$?; echo "run: exit $rc $(echo $out | tail -c 200)"
  [ "$rc" = 0 ] && [ "$out" = "$expect" ]
}
P="self=Holder(a: 67108864, b: 6) a=67108864 b=6 id=66"
CASES=(
  "reconcile-O:case_run rO reconcile_shape.swift \"PASS (result 421)\" -O"
  "reconcile-Onone:case_run rOnone reconcile_shape.swift \"PASS (result 421)\" -Onone"
  "probe-O:case_run pO upstream_probe.swift \"guard: \$P\" -O"
  "probe-Onone:case_run pOnone upstream_probe.swift \"guard: \$P\" -Onone"
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
[ "$stock_bad" -gt 0 ] || echo "::warning::stock compiler passed every SC-0003 case on this runner; the test proves less here"

if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_failures=$stock_bad/${#CASES[@]} patched_failures=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
cp "$FE" /tmp/swift-frontend.stock
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

run_all patched; patched_bad=$FAILS
echo "SUMMARY stock_failures=$stock_bad/${#CASES[@]} patched_failures=$patched_bad"
[ "$patched_bad" = 0 ]
