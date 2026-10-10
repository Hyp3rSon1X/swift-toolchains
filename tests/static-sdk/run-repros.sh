#!/bin/bash
# Runs the static reproducers built by build-repros.sh. Usage: run-repros.sh <dir> [official-attempts]
# Official-SDK binaries are informational (SC-0001 should crash with 139); patched-SDK binaries must all pass.
set -uo pipefail
D="$1"; N="${2:-6}"
[ -x /bin/echo ] && [ -x /bin/true ] && [ -x /bin/ls ] || { echo "need /bin/echo, /bin/true, /bin/ls"; exit 1; }
off_crash=0
for i in $(seq 1 "$N"); do
  timeout 60 "$D/sc0001-official" 20 16 /bin/echo dispatchio >/dev/null 2>&1; rc=$?
  echo "sc0001 official run $i: exit $rc"; [ "$rc" = 139 ] && { off_crash=$((off_crash+1)); break; }
done
pat_fail=0
for i in 1 2 3 4 5; do
  timeout 120 "$D/sc0001-patched" 30 16 /bin/echo dispatchio > /tmp/out.txt 2>&1; rc=$?
  echo "sc0001 patched run $i: exit $rc $(tail -1 /tmp/out.txt)"; [ "$rc" = 0 ] || pat_fail=$((pat_fail+1))
done
off2=0; pat2=0
for i in 1 2 3; do
  timeout 120 "$D/sc0002-official" 4000 3000 8 50 > /tmp/out.txt 2>&1; rc=$?
  echo "sc0002 official run $i: exit $rc $(tail -1 /tmp/out.txt)"; [ "$rc" = 0 ] || off2=$((off2+1))
  timeout 120 "$D/sc0002-patched" 4000 3000 8 50 > /tmp/out.txt 2>&1; rc=$?
  echo "sc0002 patched run $i: exit $rc $(tail -1 /tmp/out.txt)"; [ "$rc" = 0 ] || pat2=$((pat2+1))
done
echo "SUMMARY $(uname -m) sc0001_official_crashes=$off_crash sc0001_patched_failures=$pat_fail/5 sc0002_official_failures=$off2/3 sc0002_patched_failures=$pat2/3"
[ "$pat_fail" = 0 ] && [ "$pat2" = 0 ]
