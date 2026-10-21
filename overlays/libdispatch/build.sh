#!/bin/bash
# Runs inside the official Swift image of the matching version.
# Usage: build.sh <libdispatch-source> <out-dir>
# Builds libdispatch (C part) shared + static from the given source and checks that it is a drop-in
# replacement for the image's own copies (same exported symbols, same SONAME).
set -euo pipefail
SRC="$1"; OUT="$2"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq cmake ninja-build binutils >/dev/null

common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_SWIFT=NO -DBUILD_TESTING=OFF
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
cmake -S "$SRC" -B /tmp/b-shared "${common[@]}" -DBUILD_SHARED_LIBS=YES \
      -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON '-DCMAKE_INSTALL_RPATH=$ORIGIN' >/dev/null
cmake --build /tmp/b-shared --parallel "$(nproc)"
cmake -S "$SRC" -B /tmp/b-static "${common[@]}" -DBUILD_SHARED_LIBS=NO \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON >/dev/null
cmake --build /tmp/b-static --parallel "$(nproc)"

so=$(find /tmp/b-shared -name libdispatch.so -type f | head -1)
a=$(find /tmp/b-static -name libdispatch.a -type f | head -1)
stock_so=/usr/lib/swift/linux/libdispatch.so
stock_a=/usr/lib/swift_static/linux/libdispatch.a

mkdir -p "$OUT/usr/lib/swift/linux" "$OUT/usr/lib/swift_static/linux" "$OUT/checks"
syms() { nm "$@" 2>/dev/null | awk 'NF>=3 && $2 ~ /^[TDBRWV]$/ {print $3}' | sort -u; }
syms -D --defined-only "$stock_so" > "$OUT/checks/shared-stock.txt"
syms -D --defined-only "$so"       > "$OUT/checks/shared-built.txt"
syms -g --defined-only "$stock_a"  > "$OUT/checks/static-stock.txt"
syms -g --defined-only "$a"        > "$OUT/checks/static-built.txt"
soname() { readelf -d "$1" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p'; }

fail=0
if ! diff -u "$OUT/checks/shared-stock.txt" "$OUT/checks/shared-built.txt"; then echo "::error::shared symbol set differs"; fail=1; fi
if ! diff -u "$OUT/checks/static-stock.txt" "$OUT/checks/static-built.txt"; then echo "::error::static symbol set differs"; fail=1; fi
if [ "$(soname "$stock_so")" != "$(soname "$so")" ]; then echo "::error::SONAME differs"; fail=1; fi
[ "$fail" = 0 ] || exit 1

install -m 0755 "$so" "$OUT/usr/lib/swift/linux/libdispatch.so"
install -m 0644 "$a"  "$OUT/usr/lib/swift_static/linux/libdispatch.a"
{
  echo "component: libdispatch"
  echo "source: $(git -c safe.directory="*" -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "arch: $(uname -m)"
  echo "compiler: $(clang --version | head -1)"
  echo "shared symbols: $(wc -l < "$OUT/checks/shared-built.txt") (identical to stock)"
  echo "static symbols: $(wc -l < "$OUT/checks/static-built.txt") (identical to stock)"
  (cd "$OUT" && sha256sum usr/lib/swift/linux/libdispatch.so usr/lib/swift_static/linux/libdispatch.a)
} > "$OUT/MANIFEST-libdispatch.txt"
cat "$OUT/MANIFEST-libdispatch.txt"
