#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0006 (#92798, LLVM verifier abort) and SC-0019 (#92799, #isolation over-release) share one frontend
# fix. Builds each case with the stock compiler (informational: expected to show the bug), swaps in the
# patched swift-frontend and rebuilds (must pass). STOCK_ONLY=1 skips the swap (local stock-half check).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
W=/tmp/sc0006; rm -rf "$W"; mkdir -p "$W"
UF="-enable-upcoming-feature NonisolatedNonsendingByDefault"

# Each case compiles (and runs) one program; prints the last output line, succeeds only when correct.
case_compile_only() { # -O | -Onone
  timeout 120 swiftc $1 -parse-as-library $UF -c "$here/isolation/repro.swift" -o "$W/repro.o" 2>&1 | tail -3
  [ "${PIPESTATUS[0]}" = 0 ]
}
case_isolation() {
  local d="$W/iso$1"; mkdir -p "$d"; local out
  timeout 120 swiftc $1 $UF "$here/isolation/main.swift" -o "$d/main" 2>&1 | tail -3
  [ "${PIPESTATUS[0]}" = 0 ] || return 1
  out=$(timeout 120 "$d/main" 2>&1); echo "run: $(echo $out)"
  [ "$out" = $'main: true\nconcurrent: false' ]
}
case_lifetime() {
  local d="$W/life$1"; mkdir -p "$d"; local out rc
  timeout 120 swiftc -swift-version 6 -module-name main $1 "$here/lifetime/main.swift" -o "$d/main" 2>&1 | tail -3
  [ "${PIPESTATUS[0]}" = 0 ] || return 1
  out=$(timeout 120 "$d/main" 2>&1); rc=$?; echo "run: exit $rc $(echo $out)"
  [ "$rc" = 0 ] && [ "$out" = $'1: Optional(main.Worker)\n2: Optional(main.Worker)\nend of test()\nWorker.deinit\ndone' ]
}

CASES=(
  "compile-only-O:case_compile_only -O"
  "compile-only-Onone:case_compile_only -Onone"
  "isolation-O:case_isolation -O"
  "isolation-Onone:case_isolation -Onone"
  "lifetime-O:case_lifetime -O"
  "lifetime-Onone:case_lifetime -Onone"
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
[ "$stock_bad" -gt 0 ] || echo "::warning::stock compiler passed every SC-0006/SC-0019 case on this runner; the test proves less here"

if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_failures=$stock_bad/${#CASES[@]} patched_failures=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
cp "$FE" /tmp/swift-frontend.stock
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

run_all patched; patched_bad=$FAILS
echo "SUMMARY stock_failures=$stock_bad/${#CASES[@]} patched_failures=$patched_bad"
[ "$patched_bad" = 0 ]
