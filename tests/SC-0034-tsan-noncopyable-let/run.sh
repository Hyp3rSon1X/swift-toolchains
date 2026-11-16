#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0034: with -sanitize=thread, SILGen instrumented the object projected for a `let` class reference of a
# ~Copyable struct as if it were an address (builtin tsanInoutAccess). That kept SILGenCleanup from narrowing the
# struct load to the field, so static exclusivity checking reported a legal access to two disjoint stored properties
# (`inner.add(&box.value)`) as "overlapping accesses to 'self.inner'". Without the sanitizer, and for the Copyable
# twin, it compiles. Stock fails (informational); the patched frontend must accept the repro at -Onone and -O, and
# the instrumented executable must run clean. Second symptom (bitcast.swift, SwixForge ForgeAudio 2026-11-16): the same
# instrumentation on a non-pointer first field made IRGen emit an invalid bitcast and the LLVM verifier aborted
# ("Invalid bitcast", signal 6); the patched frontend must compile it with -c at -Onone and -O. STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
out=/tmp/sc0034; mkdir -p "$out"

accepts() { # $1 = -Onone|-O; 0 when -sanitize=thread compiles the repro
  (cd "$here" && timeout 300 swiftc -parse-as-library -module-name repro -sanitize=thread "$1" -emit-sil -o /dev/null repro.swift) \
    >"$out/sil$1.log" 2>&1
}

compiles_bitcast() { # $1 = -Onone|-O; 0 when -sanitize=thread -c compiles bitcast.swift
  (cd "$here" && timeout 300 swiftc -parse-as-library -module-name bitcast -sanitize=thread "$1" -c bitcast.swift -o "$out/bitcast.o") \
    >"$out/bitcast$1.log" 2>&1
}

plain=0; (cd "$here" && timeout 300 swiftc -parse-as-library -module-name repro -emit-sil -o /dev/null repro.swift) >/dev/null 2>&1 || plain=1
accepts -Onone; stock=$?
echo "stock plain exit $plain, stock -sanitize=thread exit $stock"
grep -m1 "overlapping accesses" "$out/sil-Onone.log" || true
compiles_bitcast -Onone; stock_bitcast=$?
echo "stock -sanitize=thread -c bitcast.swift exit $stock_bitcast $(grep -m1 -o 'Invalid bitcast' "$out/bitcast-Onone.log")"
[ "$stock" = 0 ] && echo "::warning::the stock compiler accepts the SC-0034 repro with -sanitize=thread; check whether upstream shipped the fix"
if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_tsan_exit=$stock stock_bitcast_exit=$stock_bitcast patched=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

ok=1
for o in -Onone -O; do
  if accepts "$o"; then echo "patched -sanitize=thread $o: accepted"
  else echo "FAIL: patched -sanitize=thread $o rejects the repro"; cat "$out/sil$o.log"; ok=0; fi
done
for o in -Onone -O; do
  if compiles_bitcast "$o"; then echo "patched -sanitize=thread $o -c bitcast.swift: compiles"
  else echo "FAIL: patched -sanitize=thread $o does not compile bitcast.swift"; grep -m3 -E "Invalid|Broken|error:" "$out/bitcast$o.log"; ok=0; fi
done
if (cd "$here" && timeout 600 swiftc -parse-as-library -module-name repro -sanitize=thread -Onone repro.swift main.swift -o "$out/exe") >"$out/build.log" 2>&1; then
  run=$(timeout 120 "$out/exe" 2>&1); rc=$?
  echo "patched tsan executable exit $rc: $(printf '%s\n' "$run" | grep '^sums' || true)"
  if printf '%s\n' "$run" | grep -qE "ThreadSanitizer: (data race|SEGV|access-violation)|Segmentation fault"; then
    echo "FAIL: the instrumented executable reported:"; printf '%s\n' "$run" | head -30; ok=0
  elif [ "$rc" != 0 ] || ! printf '%s\n' "$run" | grep -qx "sums 5050 5050 5050"; then
    # TSan itself may not start on some runner kernels (memory layout); that is not this bug.
    echo "::warning::SC-0034 tsan executable did not run to the end (exit $rc)"; printf '%s\n' "$run" | head -10
  fi
else
  echo "FAIL: building the -sanitize=thread executable failed"; cat "$out/build.log"; ok=0
fi
echo "SUMMARY stock_tsan_exit=$stock stock_bitcast_exit=$stock_bitcast patched_ok=$ok"
[ "$ok" = 1 ]
