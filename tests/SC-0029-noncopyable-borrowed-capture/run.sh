#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0029: an unannotated closure parameter of a ~Copyable type that the closure's contextual or inferred type borrows
# was boxed when captured, and the SIL ownership verifier aborted ("Have operand with incompatible ownership?!",
# exit 134). Adapted backport of upstream swiftlang/swift#93618 (main only; lib/SIL/IR/TypeLowering.cpp). Valid code
# (capture by a local function) must compile; capture by an escaping closure is diagnosed ("cannot be captured by an
# escaping closure since it is a borrowed parameter"). Stock is informational (expected to crash on all four files);
# the patched frontend must compile the two valid files and diagnose the two escaping ones with exit 1 (not a crash).
# STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
out=/tmp/sc0029; mkdir -p "$out"
VALID="implicit-local-function inferred-local-function"
BAD="implicit-capture inferred-escaping"
MSG="cannot be captured by an escaping closure since it is a borrowed parameter"

run_one() { # <name>; compiles <name>.swift as a library, log in $out/<name>.log; prints the exit code
  (cd "$here" && timeout 300 swiftc -parse-as-library -module-name M -emit-library "$1.swift" -o "$out/lib$1.so") \
    >"$out/$1.log" 2>&1
  echo $?
}

stock_crashes=0
for n in $VALID $BAD; do
  rc=$(run_one "$n"); echo "stock $n exit $rc"
  # the driver reports a frontend abort as exit 1, so the crash is recognized by the verifier message
  if grep -m1 "incompatible ownership" "$out/$n.log"; then stock_crashes=$((stock_crashes + 1)); fi
done
[ "$stock_crashes" = 0 ] && echo "::warning::the stock compiler no longer crashes on the SC-0029 repros; check whether it shipped swiftlang/swift#93618"
if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_crashes=$stock_crashes patched=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

ok=1
for n in $VALID; do
  rc=$(run_one "$n")
  if [ "$rc" = 0 ]; then echo "patched $n: compiles"
  else echo "FAIL: patched $n exit $rc (expected 0)"; tail -30 "$out/$n.log"; ok=0; fi
done
for n in $BAD; do
  rc=$(run_one "$n")
  if [ "$rc" = 1 ] && grep -q "$MSG" "$out/$n.log"; then echo "patched $n: diagnosed (exit 1)"
  else echo "FAIL: patched $n exit $rc (expected exit 1 and '$MSG')"; tail -30 "$out/$n.log"; ok=0; fi
done
echo "SUMMARY stock_crashes=$stock_crashes patched_ok=$ok"
[ "$ok" = 1 ]
