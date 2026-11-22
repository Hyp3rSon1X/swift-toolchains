#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0043: with -experimental-skip-non-inlinable-function-bodies-without-types (the flag SwiftPM's emit-module jobs
# use), Sema checked the effects of local functions whose bodies were skipped, so every `try #require(...)` in them
# warned "no calls to throwing functions occur within 'try' expression" (false: the macro expansion throws). Fix:
# lib/Sema/TypeCheckEffects.cpp skips local functions with skipped bodies (same fix upstream: swiftlang/swift#91082,
# main only). Stock is informational (expected to warn, 2 warnings); the patched frontend must not warn with or
# without the flag, and the file must still compile. STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
out=/tmp/sc0043; mkdir -p "$out"
SKIP=(-Xfrontend -experimental-skip-non-inlinable-function-bodies-without-types)

warnings() { # <label> <flags...>; prints the number of false warnings the typecheck produced
  local label="$1"; shift
  (cd "$here" && timeout 300 swiftc -swift-version 6 -parse-as-library -typecheck "$@" repro.swift) >"$out/$label.log" 2>&1
  local rc=$?
  [ "$rc" = 0 ] || { echo "typecheck exit $rc" >&2; sed -n 1,20p "$out/$label.log" >&2; }
  grep -c "no calls to throwing functions occur within 'try' expression" "$out/$label.log"
}

stock_skip=$(warnings stock-skip "${SKIP[@]}"); stock_plain=$(warnings stock-plain)
echo "stock with skip flag: $stock_skip false warning(s); without: $stock_plain"
[ "$stock_skip" = 0 ] && echo "::warning::the stock compiler does not warn on the SC-0043 repro; check whether it shipped swiftlang/swift#91082"
if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_skip_warnings=$stock_skip stock_plain_warnings=$stock_plain patched=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

ok=1
patched_skip=$(warnings patched-skip "${SKIP[@]}"); patched_plain=$(warnings patched-plain)
echo "patched with skip flag: $patched_skip warning(s); without: $patched_plain"
[ "$patched_skip" = 0 ] && [ "$patched_plain" = 0 ] || { echo "FAIL: patched compiler still warns"; ok=0; }
if (cd "$here" && timeout 300 swiftc -swift-version 6 -parse-as-library -emit-library -module-name M repro.swift -o "$out/libM.so") >"$out/lib.log" 2>&1
then echo "patched full compile: ok"; else echo "FAIL: patched full compile failed"; tail -20 "$out/lib.log"; ok=0; fi
echo "SUMMARY stock_skip_warnings=$stock_skip stock_plain_warnings=$stock_plain patched_skip_warnings=$patched_skip patched_ok=$ok"
[ "$ok" = 1 ]
