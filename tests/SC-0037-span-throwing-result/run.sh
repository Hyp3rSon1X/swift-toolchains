#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0037 (swiftlang/swift#92412): `total(try make(n).span)`, the Array result of a throwing call borrowed as a Span in
# another call's argument. LifetimeDependenceScopeFixup began the array's extended scope at the try_apply, so it put a
# destroy of the result into the error block, and every optimization level aborted in the LLVM verifier ("Instruction
# does not dominate all uses"). Stock is informational (expected to crash); the patched frontend must compile the repro
# at -Onone (also with the SIL verifier), -O, -Osize and -O -wmo, and the -Onone and -O executables must print the
# expected lines (normal and error paths). STOCK_ONLY=1 runs the checks with the stock compiler only.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
out=/tmp/sc0037; mkdir -p "$out"
EXPECT=$'run 10\nrun -1 negative\ntyped 21\ntyped -1 negative\nload 4\nload -2 negative\ntop 6'

compiles() { # <label> <flags...>; 0 when the repro compiles as a library
  local label="$1"; shift
  (cd "$here" && timeout 300 swiftc "$@" -parse-as-library -module-name M -emit-library repro.swift -o "$out/libM-$label.so") \
    >"$out/lib-$label.log" 2>&1
}

check() { # <who>; runs the whole matrix with the current compiler, 0 when everything passes
  local who="$1" ok=1 mode flags run rc
  for mode in Onone Onone-silverify O Osize O-wmo; do
    case $mode in
      Onone-silverify) flags=(-Onone -Xfrontend -sil-verify-all);;
      O-wmo) flags=(-O -wmo);;
      *) flags=(-"$mode");;
    esac
    if compiles "$mode" "${flags[@]}"; then echo "$who ${flags[*]} library: compiles"
    else echo "FAIL: $who ${flags[*]} does not compile the repro"; grep -m3 -E "dominate|verification failed|error:" "$out/lib-$mode.log"; ok=0; fi
  done
  for mode in Onone O; do
    if (cd "$here" && timeout 300 swiftc -"$mode" -module-name M repro.swift main.swift -o "$out/exe-$mode") >"$out/exe-$mode.log" 2>&1; then
      run=$(timeout 60 "$out/exe-$mode" 2>&1); rc=$?
      echo "$who -$mode executable exit $rc: $(echo "$run" | tr '\n' ';')"
      if [ "$rc" != 0 ] || [ "$run" != "$EXPECT" ]; then echo "FAIL: $who -$mode expected exit 0 and the 7 expected lines"; ok=0; fi
    else
      echo "FAIL: $who building the -$mode executable failed"; grep -m3 -E "dominate|error:" "$out/exe-$mode.log"; ok=0
    fi
  done
  [ "$ok" = 1 ]
}

check stock; stock=$?
[ "$stock" = 0 ] && echo "::warning::the stock compiler passes the SC-0037 gate; check whether upstream fixed swiftlang/swift#92412"
if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_ok=$((1 - (stock != 0))) patched=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

check patched; ok=$?
echo "SUMMARY stock_ok=$((1 - (stock != 0))) patched_ok=$((1 - (ok != 0)))"
[ "$ok" = 0 ]
