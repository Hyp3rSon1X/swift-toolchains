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

# Valgrind and Process.run() cannot complete a launch together: valgrind keeps its own descriptors at the top of the
# RLIMIT_NOFILE range (visible in /proc/self/fd, e.g. 4084..4090 for a limit of 4096), so findMaximumOpenFD() returns
# that number and Foundation adds a posix_spawn close action for every descriptor up to it, valgrind's own included.
# Valgrind refuses to close those, the spawn fails with EBADF, and the cost grows with the limit (about 5 s at 65536,
# no completion within 120 s at 1048576, the usual container default). Stock and patched libraries behave the same
# (SC-0017). So the valgrind step runs with a small limit (fast, deterministic) and accepts exactly two outcomes:
#   clean   exit 0
#   ebadf   run() threw EBADF, i.e. the launch itself was refused by valgrind, after findMaximumOpenFromProcSelfFD ran
# Anything else (timeout, crash, other failure text, valgrind error exit without the EBADF signature) is "abnormal".
# The invalid-read count only certifies the patch when the outcome is not abnormal.
VG_NOFILE=4096
vg() {  # vg <binary>: sets vg_reads (valgrind "Invalid read"s attributed to findMaximumOpenFromProcSelfFD) and vg_outcome
  local t0=$SECONDS rc
  ( ulimit -n "$VG_NOFILE"; timeout -s KILL 60 valgrind -q --num-callers=4 --error-exitcode=99 "$1" 1500 1 1 1 > /tmp/vg.out 2>&1 )
  rc=$?
  vg_reads=$(grep -A3 'Invalid read' /tmp/vg.out | grep -c findMaximumOpenFromProcSelfFD)
  if [ "$rc" = 0 ]; then vg_outcome=clean
  elif grep -q 'Bad file descriptor' /tmp/vg.out && grep -q 'run() threw' /tmp/vg.out; then vg_outcome=ebadf
  else vg_outcome=abnormal; fi
  echo "valgrind $1: exit $rc outcome=$vg_outcome $((SECONDS-t0))s nofile=$VG_NOFILE, $(wc -l < /tmp/vg.out) output lines, first: $(head -c 300 /tmp/vg.out | tr '\n' ' ')" >&2
}

stock_fail=0
for i in 1 2 3; do
  timeout 120 /tmp/repro 4000 3000 8 50 > /tmp/out.txt 2>&1; rc=$?
  echo "stock run $i: exit $rc $(tail -1 /tmp/out.txt)"; [ "$rc" = 0 ] || stock_fail=$((stock_fail+1))
done
vg /tmp/repro; stock_reads=$vg_reads; echo "stock valgrind: $stock_reads invalid reads in findMaximumOpenFromProcSelfFD (outcome $vg_outcome)"
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
vg /tmp/repro; patched_reads=$vg_reads; patched_vg_outcome=$vg_outcome
echo "patched valgrind: $patched_reads invalid reads in findMaximumOpenFromProcSelfFD (outcome $patched_vg_outcome)"

echo "SUMMARY stock_failures=$stock_fail/3 stock_valgrind_reads=$stock_reads patched_failures=$patched_fail/4 patched_valgrind_reads=$patched_reads patched_valgrind_outcome=$patched_vg_outcome"
[ "$patched_fail" = 0 ] && [ "$patched_reads" = 0 ] && [ "$patched_vg_outcome" != abnormal ]
