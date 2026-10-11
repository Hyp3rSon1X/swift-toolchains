#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0014: a signal source for a signal whose disposition was reset to SIG_IGN after the first source registered never
# fired on Linux (libdispatch installed its sigaction only once per signal). Stock run informational (case 2 should be
# lost); patched run must deliver cases 0-2. Case 3 (SIG_IGN set while a source is live) is a documented limitation.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="$1"
swiftc -O "$here/sig.swift" -o /tmp/sig || exit 1
run() { timeout 60 /tmp/sig > /tmp/sig.out 2>&1; sed "s/^/$1 /" /tmp/sig.out | grep -E '^[a-z0-9]+ [0-3] '; }
run stock
grep -q '^2 .*NOT delivered' /tmp/sig.out || echo "::warning::stock libdispatch delivered case 2; the test proves less on this runner"
stock_lost=$(grep -c 'NOT delivered' /tmp/sig.out)

cp "$overlay/usr/lib/swift/linux/libdispatch.so" /usr/lib/swift/linux/libdispatch.so
ldd /tmp/sig | grep libdispatch
patched_fail=0
for i in 1 2 3; do
  run "patched$i"
  n=$(grep -cE '^[012] .*: delivered$' /tmp/sig.out); [ "$n" = 3 ] || patched_fail=$((patched_fail+1))
done
echo "SUMMARY stock_lost=$stock_lost/4 patched_failures=$patched_fail/3 (cases 0-2 must be delivered; case 3 is a known limitation)"
[ "$patched_fail" = 0 ]
