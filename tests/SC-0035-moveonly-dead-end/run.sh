#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0035: the move-only address checker put a destroy_addr of an inout ~Copyable field into a dead-end try_apply
# error block (one that ends in unreachable); after inlining it landed inside a load_borrow scope, and -O builds
# aborted with "SIL verification failed: read-only scope invalidated by a local write". Backport of upstream
# swiftlang/swift#90163 (main only). Stock is informational (expected to crash at -O and -Osize); the patched
# frontend must compile the repro at -Onone, -O, -Osize and -O -wmo, and the -O executable must print the
# expected line. STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
out=/tmp/sc0035; mkdir -p "$out"
EXPECT="count 3 x 2"

compiles() { # <label> <flags...>; 0 when the repro compiles as a library
  local label="$1"; shift
  (cd "$here" && timeout 300 swiftc "$@" -parse-as-library -module-name M -emit-library repro.swift -o "$out/libM.so") \
    >"$out/lib-$label.log" 2>&1
}

compiles O -O; stock=$?
echo "stock -O library exit $stock"
grep -m1 "SIL verification failed" "$out/lib-O.log" || true
[ "$stock" = 0 ] && echo "::warning::the stock compiler compiles the SC-0035 repro at -O; check whether upstream shipped swiftlang/swift#90163"
if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_O_exit=$stock patched=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

ok=1
for mode in Onone O Osize O-wmo; do
  case $mode in O-wmo) flags=(-O -wmo);; *) flags=(-"$mode");; esac
  if compiles "$mode" "${flags[@]}"; then echo "patched ${flags[*]} library: compiles"
  else echo "FAIL: patched ${flags[*]} does not compile the repro"; tail -30 "$out/lib-$mode.log"; ok=0; fi
done
if (cd "$here" && timeout 300 swiftc -O -module-name M repro.swift main.swift -o "$out/exe") >"$out/exe.log" 2>&1; then
  run=$(timeout 60 "$out/exe" 2>&1); rc=$?
  echo "patched -O executable exit $rc: $run"
  if [ "$rc" != 0 ] || [ "$run" != "$EXPECT" ]; then echo "FAIL: expected exit 0 and '$EXPECT'"; ok=0; fi
else
  echo "FAIL: building the -O executable failed"; tail -30 "$out/exe.log"; ok=0
fi
echo "SUMMARY stock_O_exit=$stock patched_ok=$ok"
[ "$ok" = 1 ]
