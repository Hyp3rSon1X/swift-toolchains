#!/bin/bash
# Runs inside the official Swift image of the matching version.
# Usage: build.sh <corelibs-foundation-src> <deps-dir> <out-dir>
#   <deps-dir> holds checkouts named swift-foundation, swift-foundation-icu, swift-collections at the revisions
#   the official release pins (swift utils/update_checkout/update-checkout-config.json).
# Rebuilds swift-corelibs-foundation (shared + static, Release) against the image's own toolchain, the way
# build-script-impl does (-D_SwiftFoundation_SourceDIR etc.), and keeps only libFoundation.so / libFoundation.a:
# the patched code lives in swift-corelibs-foundation's own Sources/Foundation, the sibling libraries
# (FoundationEssentials, FoundationInternationalization, _FoundationICU, FoundationNetworking, FoundationXML)
# are built from unchanged sources and stay the image's copies. The overlay is a drop-in only if the exported
# symbols and SONAME of both kept libraries equal the image's.
set -euo pipefail
SRC="$1"; DEPS="$2"; OUT="$3"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq cmake ninja-build binutils libxml2-dev libcurl4-openssl-dev >/dev/null

common=(-G Ninja -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_Swift_COMPILER="$(command -v swiftc)"
        -DSwiftFoundation_MACRO=/usr/lib/swift/host/plugins
        -D_SwiftFoundation_SourceDIR="$DEPS/swift-foundation"
        -D_SwiftFoundationICU_SourceDIR="$DEPS/swift-foundation-icu"
        -D_SwiftCollections_SourceDIR="$DEPS/swift-collections"
        -DENABLE_TESTING=NO)
# $ORIGIN install RPATH keeps the stock RUNPATH so the replacement resolves its siblings like the image's copy.
cmake -S "$SRC" -B /tmp/b-shared "${common[@]}" -DBUILD_SHARED_LIBS=YES \
      -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON '-DCMAKE_INSTALL_RPATH=$ORIGIN' >/dev/null
cmake --build /tmp/b-shared --target Foundation --parallel "$(nproc)"
cmake -S "$SRC" -B /tmp/b-static "${common[@]}" -DBUILD_SHARED_LIBS=NO >/dev/null
cmake --build /tmp/b-static --target Foundation --parallel "$(nproc)"

so=$(find /tmp/b-shared -name libFoundation.so -type f | head -1)
a=$(find /tmp/b-static -name libFoundation.a -type f | head -1)
stock_so=/usr/lib/swift/linux/libFoundation.so
stock_a=/usr/lib/swift_static/linux/libFoundation.a

mkdir -p "$OUT/usr/lib/swift/linux" "$OUT/usr/lib/swift_static/linux" "$OUT/checks"
syms() { nm "$@" 2>/dev/null | awk 'NF>=3 && $2 ~ /^[TDBRWV]$/ {print $3}' | sort -u; }
syms -D --defined-only "$stock_so" > "$OUT/checks/foundation-shared-stock.txt"
syms -D --defined-only "$so"       > "$OUT/checks/foundation-shared-built.txt"
syms -g --defined-only "$stock_a"  > "$OUT/checks/foundation-static-stock.txt"
syms -g --defined-only "$a"        > "$OUT/checks/foundation-static-built.txt"
soname() { readelf -d "$1" | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p'; }
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort; }

fail=0
if ! diff -u "$OUT/checks/foundation-shared-stock.txt" "$OUT/checks/foundation-shared-built.txt"; then echo "::error::libFoundation.so symbol set differs"; fail=1; fi
if ! diff -u "$OUT/checks/foundation-static-stock.txt" "$OUT/checks/foundation-static-built.txt"; then echo "::error::libFoundation.a symbol set differs"; fail=1; fi
if [ "$(soname "$stock_so")" != "$(soname "$so")" ]; then echo "::error::SONAME differs"; fail=1; fi
if ! diff <(needed "$stock_so") <(needed "$so"); then echo "::error::NEEDED set differs"; fail=1; fi
[ "$fail" = 0 ] || exit 1

install -m 0755 "$so" "$OUT/usr/lib/swift/linux/libFoundation.so"
install -m 0644 "$a"  "$OUT/usr/lib/swift_static/linux/libFoundation.a"
{
  echo "component: foundation (swift-corelibs-foundation: libFoundation.so, libFoundation.a)"
  echo "source: $(git -c safe.directory="*" -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
  for d in swift-foundation swift-foundation-icu swift-collections; do echo "dep $d: $(git -c safe.directory="*" -C "$DEPS/$d" rev-parse HEAD 2>/dev/null || echo unknown)"; done
  echo "arch: $(uname -m)"
  echo "compiler: $(clang --version | head -1); $(swiftc --version 2>&1 | head -1)"
  echo "shared symbols: $(wc -l < "$OUT/checks/foundation-shared-built.txt") (identical to stock)"
  echo "static symbols: $(wc -l < "$OUT/checks/foundation-static-built.txt") (identical to stock)"
  (cd "$OUT" && sha256sum usr/lib/swift/linux/libFoundation.so usr/lib/swift_static/linux/libFoundation.a)
} > "$OUT/MANIFEST-foundation.txt"
cat "$OUT/MANIFEST-foundation.txt"
