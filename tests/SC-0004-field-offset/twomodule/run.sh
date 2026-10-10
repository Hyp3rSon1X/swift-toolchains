#!/bin/sh
# Usage: run.sh <swiftc> <outdir> [extra swiftc flags...]
# Builds Lib as a library-evolution dylib and main with -O -wmo, then runs it.
set -e
SWIFTC="$1"; OUT="$2"; shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"
case "$(uname)" in
  Darwin) LIB=libLib.dylib; RP="-Xlinker -rpath -Xlinker $OUT" ;;
  *)      LIB=libLib.so;    RP="-Xlinker -rpath=$OUT" ;;
esac
"$SWIFTC" "$@" -O -wmo -parse-as-library -enable-library-evolution -emit-library \
  -module-name Lib -emit-module -emit-module-path "$OUT/Lib.swiftmodule" \
  "$HERE/Lib.swift" -o "$OUT/$LIB"
"$SWIFTC" "$@" -O -wmo -module-name main -I "$OUT" -L "$OUT" -lLib $RP \
  "$HERE/main.swift" -o "$OUT/main"
"$OUT/main"
