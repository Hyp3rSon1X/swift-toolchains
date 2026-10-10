#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# The stock library does not crash observably in practice (the over-read is rare to fault), so the stock part is
# informational; valgrind makes the over-read itself visible. Patched runs must be clean.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="$1"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq valgrind >/dev/null
swiftc -O "$here/main.swift" -o /tmp/repro || exit 1
swiftc -O -static-stdlib "$here/main.swift" -o /tmp/repro-static || exit 1   # links /usr/lib/swift_static/linux/libFoundation.a

vg() {  # vg <binary>: count valgrind "Invalid read"s attributed to findMaximumOpenFromProcSelfFD (40 s cap)
  timeout -s KILL 40 valgrind -q --num-callers=4 "$1" 1500 1 1 1 > /tmp/vg.out 2>&1
  echo "valgrind $1: $(wc -l < /tmp/vg.out) output lines, first: $(head -c 300 /tmp/vg.out | tr '\n' ' ')" >&2
  grep -A3 'Invalid read' /tmp/vg.out | grep -c findMaximumOpenFromProcSelfFD
}

stock_fail=0
for i in 1 2 3; do
  timeout 120 /tmp/repro 4000 3000 8 50 > /tmp/out.txt 2>&1; rc=$?
  echo "stock run $i: exit $rc $(tail -1 /tmp/out.txt)"; [ "$rc" = 0 ] || stock_fail=$((stock_fail+1))
done
stock_reads=$(vg /tmp/repro); echo "stock valgrind: $stock_reads invalid reads in findMaximumOpenFromProcSelfFD"
[ "$stock_reads" -gt 0 ] || echo "::warning::valgrind saw no over-read on the stock library; the reproducer proves less on this runner"

cp "$overlay/usr/lib/swift/linux/libFoundation.so" /usr/lib/swift/linux/libFoundation.so
cp "$overlay/usr/lib/swift_static/linux/libFoundation.a" /usr/lib/swift_static/linux/libFoundation.a
swiftc -O -static-stdlib "$here/main.swift" -o /tmp/repro-static || exit 1
ldd /tmp/repro | grep -E 'libFoundation\.so'
sha256sum /usr/lib/swift/linux/libFoundation.so "$overlay/usr/lib/swift/linux/libFoundation.so"

patched_fail=0
for i in 1 2 3; do
  timeout 120 /tmp/repro 4000 3000 8 50 > /tmp/out.txt 2>&1; rc=$?
  echo "patched run $i: exit $rc $(tail -1 /tmp/out.txt)"; [ "$rc" = 0 ] || patched_fail=$((patched_fail+1))
done
timeout 120 /tmp/repro-static 4000 1500 8 50 > /tmp/out.txt 2>&1; rc=$?
echo "patched static run: exit $rc $(tail -1 /tmp/out.txt)"; [ "$rc" = 0 ] || patched_fail=$((patched_fail+1))
patched_reads=$(vg /tmp/repro); echo "patched valgrind: $patched_reads invalid reads in findMaximumOpenFromProcSelfFD"

echo "SUMMARY stock_failures=$stock_fail/3 stock_valgrind_reads=$stock_reads patched_failures=$patched_fail/4 patched_valgrind_reads=$patched_reads"
[ "$patched_fail" = 0 ] && [ "$patched_reads" = 0 ]
