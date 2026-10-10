#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# Builds the two reproducers with the stock compiler (informational: they must show the miscompile),
# swaps in the patched swift-frontend and rebuilds them (must be intact), then checks that the
# patched frontend accepts the official stdlib/Foundation modules (SwiftPM build + run).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="$1"
FE=/usr/bin/swift-frontend

# Prints the last output line; succeeds only when the reproducer exits 0 and reports an intact isa.
run_case() { # <label> <case> <outdir>
  local label="$1" c="$2" out="$3" log rc
  rm -rf "$out"; mkdir -p "$out"
  log=$(sh "$here/$c/run.sh" swiftc "$out" 2>&1); rc=$?
  echo "$label $c: exit $rc $(echo "$log" | tail -1)"
  [ "$rc" = 0 ] && echo "$log" | grep -q 'intact=true'
}

stock_bad=0
for c in twomodule generic; do
  run_case stock "$c" "/tmp/stock-$c" || stock_bad=$((stock_bad+1))
done
[ "$stock_bad" -gt 0 ] || echo "::warning::stock compiler did not miscompile either reproducer on this runner; the test proves less here"

stock_ver=$("$FE" -version 2>&1 | head -1)
cp "$FE" /tmp/swift-frontend.stock
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
patched_ver=$("$FE" -version 2>&1 | head -1)
echo "stock frontend:   $stock_ver"
echo "patched frontend: $patched_ver"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

patched_fail=0
for c in twomodule generic; do
  run_case patched "$c" "/tmp/patched-$c" || patched_fail=$((patched_fail+1))
done

# The patched frontend must accept the official stdlib / Foundation / Dispatch interfaces and macros:
# SwiftPM package (Foundation, Dispatch, concurrency, a macro-free test-style target), release build, run.
rm -rf /tmp/smoke; mkdir -p /tmp/smoke && cd /tmp/smoke && swift package init --type executable --name Smoke >/dev/null
cat > Sources/main.swift <<'S'
import Foundation
import Dispatch
struct Item: Codable, Hashable { var id = UUID(); var name: String }
let items = (0..<100).map { Item(name: "n\($0)") }
let data = try JSONEncoder().encode(items)
let back = try JSONDecoder().decode([Item].self, from: data)
let group = DispatchGroup()
let lock = NSLock(); var n = 0
for _ in 0..<8 { DispatchQueue.global().async(group: group) { lock.lock(); n += 1; lock.unlock() } }
group.wait()
func f() async -> Int { 42 }
let sem = DispatchSemaphore(value: 0); var r = 0
Task { r = await f(); sem.signal() }
sem.wait()
print("smoke ok \(back.count == 100 && n == 8 && r == 42) \(back.count) \(n) \(r) \(items.first!.name)")
S
smoke=ok
swift build -c release >/tmp/smoke.log 2>&1 || { smoke="build-failed"; tail -30 /tmp/smoke.log; }
if [ "$smoke" = ok ]; then out=$(.build/release/Smoke 2>&1) || smoke="run-failed"; echo "$out"; echo "$out" | grep -q 'smoke ok true' || smoke="wrong-output"; fi
echo "swift build smoke: $smoke"

echo "SUMMARY stock_miscompiled=$stock_bad/2 patched_failures=$patched_fail smoke=$smoke"
[ "$patched_fail" = 0 ] && [ "$smoke" = ok ]
