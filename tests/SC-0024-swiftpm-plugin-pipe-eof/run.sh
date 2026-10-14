#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <libdispatch/Foundation overlay dir> <patched swift-package>
# SC-0024: with libdispatch's epoll backend fixed (SC-0001, upstream PR #965), a read source on a pipe whose writer has
# closed keeps firing until it is cancelled. SwiftPM 6.4.0's DefaultPluginScriptRunner never removes the
# readabilityHandlers on a plugin's stdout and stderr, so after a plugin exited they spin at end-of-file until the process
# ends (+68 % CPU per re-plan of a package with a build-tool plugin). The patched swift-package removes them at EOF.
#
# The package in pkg/ has a build-tool plugin whose command sleeps 3 s, so `swift build` lives long after the plugin
# process exited. strace counts the reads that return 0 (EOF) per pipe (eof_reads.py, from SwixForge's verify tooling).
# A healthy reader sees at most 2; a spinning one thousands. Stock swift-package with the patched libdispatch is
# informational (it should spin); the patched swift-package must stay below the limit in every round.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="$1"; patched_spm="$(realpath "$2")"
limit=20
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq strace python3 >/dev/null || exit 1

# build <label> -> strace log in /tmp/<label>.strace, builds a fresh copy so the plugin runs every time
build() {
  local label=$1 dir=/tmp/pkg-$1
  rm -rf "$dir"; cp -r "$here/pkg" "$dir"
  ( cd "$dir" && strace -f --seccomp-bpf -qq -yy -e trace=read -o "/tmp/$label.strace" swift build ) > "/tmp/$label.build" 2>&1
  local rc=$?
  local got; got=$("$dir/.build/debug/demo" 2>&1)
  if [ "$rc" != 0 ] || [ "$got" != 42 ]; then echo "::error::$label: build exit $rc, demo printed '$got'"; tail -20 "/tmp/$label.build"; return 1; fi
}
# eof <label> -> prints the summary line, sets max_eof
eof() {
  python3 -I "$here/eof_reads.py" "/tmp/$1.strace" "$limit" > "/tmp/$1.eof"; true
  max_eof=$(sed -n 's/.*max on one pipe: \([0-9]*\).*/\1/p' "/tmp/$1.eof")
  echo "$1: $(head -1 "/tmp/$1.eof")"
}

stock_spm=/tmp/swift-package.stock
cp /usr/bin/swift-package "$stock_spm"

build stock-stocklib || exit 1; eof stock-stocklib
stock_lib_max=$max_eof

# Patched runtime libraries, as in the shipped image (libdispatch with PR #965; Foundation as built for this release).
[ -d "$overlay/usr/lib" ] && cp -a "$overlay/usr/lib/." /usr/lib/
ldd "$stock_spm" | grep libdispatch

build stock-patchedlib || exit 1; eof stock-patchedlib
stock_max=$max_eof
sed -n '2,12p' /tmp/stock-patchedlib.eof
[ "$stock_max" -gt "$limit" ] || echo "::warning::stock swift-package did not spin on the patched libdispatch (max $stock_max EOF reads); the test proves less on this runner"

cp "$patched_spm" /usr/bin/swift-package; chmod 0755 /usr/bin/swift-package
timeout 60 /usr/bin/swift-package --version
patched_fail=0; patched_worst=0
for i in 1 2 3; do
  build "patched$i" || { patched_fail=$((patched_fail+1)); continue; }
  eof "patched$i"
  [ "$max_eof" -le "$limit" ] || { patched_fail=$((patched_fail+1)); sed -n '2,12p' "/tmp/patched$i.eof"; }
  [ "$max_eof" -le "$patched_worst" ] || patched_worst=$max_eof
done
echo "SUMMARY stock_stocklib_max_eof_reads=$stock_lib_max stock_patchedlib_max_eof_reads=$stock_max patched_max_eof_reads=$patched_worst patched_failures=$patched_fail/3 (limit $limit)"
[ "$patched_fail" = 0 ]
