#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# Stock runs are informational (they should crash); patched runs must all pass.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="$1"
swiftc -O "$here/main.swift" -o /tmp/repro || exit 1

stock_crashes=0
for i in 1 2 3; do
  timeout 90 /tmp/repro 20 16 /bin/echo dispatchio >/dev/null 2>&1; rc=$?
  echo "stock run $i: exit $rc"; [ "$rc" = 139 ] && stock_crashes=$((stock_crashes+1))
done
[ "$stock_crashes" -gt 0 ] || echo "::warning::stock libdispatch did not crash in 3 runs; the reproducer proves less on this runner"

cp "$overlay/usr/lib/swift/linux/libdispatch.so" /usr/lib/swift/linux/libdispatch.so
ldd /tmp/repro | grep libdispatch
patched_fail=0
for i in 1 2 3 4 5; do
  timeout 120 /tmp/repro 30 16 /bin/echo dispatchio > /tmp/out.txt 2>&1; rc=$?
  echo "patched run $i: exit $rc $(tail -1 /tmp/out.txt)"; [ "$rc" = 0 ] || patched_fail=$((patched_fail+1))
done

# SwiftPM smoke test with the patched library (swift-build planning is what crashed in CI).
mkdir -p /tmp/smoke && cd /tmp/smoke && swift package init --type executable --name Smoke >/dev/null
smoke=ok
for i in 1 2 3; do rm -rf .build; swift build -c release >/tmp/smoke.log 2>&1 || { smoke="failed (run $i)"; tail -20 /tmp/smoke.log; break; }; done
echo "swift build smoke x3: $smoke"

echo "SUMMARY stock_crashes=$stock_crashes/3 patched_failures=$patched_fail/5 smoke=$smoke"
[ "$patched_fail" = 0 ] && [ "$smoke" = ok ]
