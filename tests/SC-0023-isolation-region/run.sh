#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0023: our SC-0006 fix made the type checker treat every call that passes #isolation to an isolated
# parameter in nonisolated(nonsending) code as an isolation crossing, so region isolation rejected
# non-Sendable results ("non-Sendable ... returned from actor-isolated ... to nonisolated context"):
# for await over non-Sendable elements, Swift Testing's #expect(processExitsWith:). A regression of
# hyp3r compilers only: stock 6.4.0 accepts every case. Each case runs through SIL (region isolation
# runs there, -typecheck does not show it) with and without NonisolatedNonsendingByDefault; stock is
# informational, the patched frontend must accept all. STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
UF="-enable-upcoming-feature NonisolatedNonsendingByDefault"
FILES=(repro-forawait.swift repro-class.swift repro-explicit.swift repro.swift repro-sendable.swift)

run_all() { # <label>; sets FAILS
  FAILS=0
  local f mode flags log ok
  for f in "${FILES[@]}"; do
    for mode in nonsending default; do
      flags="-swift-version 6"; [ "$mode" = nonsending ] && flags="$flags $UF"
      log=$(timeout 120 swiftc $flags -emit-sil -o /dev/null "$here/$f" 2>&1); ok=$?
      echo "$1 $f [$mode]: $([ $ok = 0 ] && echo pass || echo FAIL) | $(echo "$log" | grep -m1 'error:' | cut -c1-200)"
      [ $ok = 0 ] || FAILS=$((FAILS+1))
    done
  done
}

total=$(( ${#FILES[@]} * 2 ))
run_all stock; stock_bad=$FAILS
[ "$stock_bad" = 0 ] || echo "::warning::the stock compiler rejected $stock_bad SC-0023 case(s); stock is expected to accept all"

if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_failures=$stock_bad/$total patched_failures=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
cp "$FE" /tmp/swift-frontend.stock
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

run_all patched; patched_bad=$FAILS
echo "SUMMARY stock_failures=$stock_bad/$total patched_failures=$patched_bad"
[ "$patched_bad" = 0 ]
