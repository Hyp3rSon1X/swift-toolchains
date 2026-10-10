#!/bin/bash
# Runs inside the official Swift image of the matching version (the toolchain that matches the SDK bundle).
# Usage: build.sh <arch> <sdk-dir> <libdispatch-src> <foundation-src> <deps-dir> <out-dir>
#   <arch>      x86_64 | aarch64 (the SDK triple <arch>-swift-linux-musl; cross builds work, CI builds natively)
#   <sdk-dir>   the extracted official Static Linux SDK (the directory holding swift-sdk.json and musl-*.sdk/)
#   <deps-dir>  checkouts named swift-foundation, swift-foundation-icu, swift-collections (release-pinned)
# Rebuilds libdispatch.a and libFoundation.a for the musl static SDK the way swiftlang/swift-docker
# swift-ci/sdks/static-linux/scripts/build.sh does (same CMake options, RelWithDebInfo, ENABLE_SWIFT=YES for
# libdispatch, BUILD_FULLY_STATIC=YES and SWIFT_SYSTEM_NAME=linux-static for Foundation), against the
# SDK's own sysroot, Swift modules and clang headers, and requires the global defined symbols of both
# archives to equal the official bundle's. Only these two archives are produced; libswiftDispatch.a and the
# sibling Foundation archives are unchanged by the fixes and stay the official ones.
set -euo pipefail
ARCH="$1"; SDKDIR="$(cd "$2" && pwd)"; DSRC="$3"; FSRC="$4"; DEPS="$5"; OUT="$6"
export DEBIAN_FRONTEND=noninteractive
# CMake >= 3.29 is required for fidelity: Foundation's CMakeLists sets policy CMP0157 NEW only when it exists, which
# switches RelWithDebInfo Swift builds from whole-module to the incremental (per-file) mode the official SDK was
# built with. With Ubuntu's 3.28 the archive has a different symbol set (2000+ symbols differ).
apt-get update -qq && apt-get install -y -qq ninja-build binutils python3-pip >/dev/null
pip install -q --break-system-packages cmake==3.31.6 2>&1 | grep -v 'WARNING: Running pip' || true
hash -r; cmake --version | head -1

TRIPLE="$ARCH-swift-linux-musl"
# Work on a private copy: cmake and swiftc may write into the sysroot tree, and a local mount may not allow symlinks.
SDK=/tmp/sdk; mkdir -p "$SDK"; cp -a "$SDKDIR/musl-"*.sdk/"$ARCH" "$SDK/root"
ROOT="$SDK/root"; STATIC="$ROOT/usr/lib/swift_static"; RES="$ROOT/usr/lib/swift/clang"
for t in "$STATIC/linux-static/libdispatch.a" "$STATIC/linux-static/libFoundation.a"; do [ -f "$t" ] || { echo "::error::missing $t"; exit 1; }; done
cp -a "$STATIC/linux-static/libdispatch.a" "$SDK/official-libdispatch.a"
cp -a "$STATIC/linux-static/libFoundation.a" "$SDK/official-libFoundation.a"

# The shipped SDK already contains what libdispatch and Foundation install (headers, module maps, modules,
# archives). The official build compiled them before installing, so a second CDispatch/CoreFoundation module
# map next to the source tree's would be a redefinition. Remove them from the private copy.
rm -rf "$STATIC/Block" "$STATIC/dispatch" "$STATIC/os" "$STATIC/CoreFoundation" "$STATIC/_FoundationCShims" "$STATIC/_foundation_unicode"
rm -rf "$STATIC"/linux-static/{Dispatch,Foundation*,_Foundation*}.swiftmodule
rm -f "$STATIC"/linux-static/{libdispatch,libswiftDispatch,libBlocksRuntime,libCoreFoundation,libFoundation*,lib_Foundation*,lib_CF*}.a

cat > /tmp/toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $ARCH)
set(CMAKE_ASM_COMPILER_TARGET $TRIPLE)
set(CMAKE_C_COMPILER_TARGET $TRIPLE)
set(CMAKE_CXX_COMPILER_TARGET $TRIPLE)
set(CMAKE_Swift_COMPILER_TARGET $TRIPLE)
set(CMAKE_SYSROOT $ROOT)
# Same link setup as the official build's clang .cfg: configure checks (check_function_exists ...) must really
# link, against the SDK's own libc++/libunwind/compiler-rt, or they all report success.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -fuse-ld=lld -rtlib=compiler-rt -stdlib=libc++ -unwindlib=libunwind -lc++ -lc++abi")
set(CMAKE_C_COMPILER /usr/bin/clang)
set(CMAKE_CXX_COMPILER /usr/bin/clang++)
set(CMAKE_ASM_COMPILER /usr/bin/clang)
set(CMAKE_C_FLAGS_INIT "-resource-dir $RES")
set(CMAKE_CXX_FLAGS_INIT "-resource-dir $RES -stdlib=libc++")
set(CMAKE_ASM_FLAGS_INIT "-resource-dir $RES")
set(CMAKE_FIND_ROOT_PATH $ROOT)
set(CMAKE_Swift_COMPILER /usr/bin/swiftc)
set(CMAKE_Swift_FLAGS "-static-stdlib -use-ld=/usr/bin/ld.lld -sdk $ROOT -target $TRIPLE -resource-dir $STATIC -Xclang-linker -resource-dir -Xclang-linker $RES")
EOF

B=/tmp/b; mkdir -p "$B"
# --- libdispatch (static-stdlib-args.lnk must not list -ldispatch/-lBlocksRuntime while it is built) ---
lnk="$STATIC/linux-static/static-stdlib-args.lnk"
cp "$lnk" "$lnk.bak"; grep -v '\(-ldispatch\|-lBlocksRuntime\)' "$lnk.bak" > "$lnk"
cmake -G Ninja -S "$DSRC" -B "$B/dispatch" -DCMAKE_TOOLCHAIN_FILE=/tmp/toolchain.cmake \
      -DCMAKE_REQUIRED_DEFINITIONS=-D_GNU_SOURCE -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_INSTALL_PREFIX="$ROOT/usr" -DCMAKE_Swift_COMPILER_WORKS=YES \
      -DBUILD_SHARED_LIBS=NO -DENABLE_SWIFT=YES -DSWIFT_SYSTEM_NAME=linux-static -DBUILD_TESTING=OFF >/dev/null
cmake --build "$B/dispatch" --parallel "$(nproc)"

# --- Foundation (built against the build-tree dispatch, as upstream does) ---
cmake -G Ninja -S "$FSRC" -B "$B/foundation" -DCMAKE_TOOLCHAIN_FILE=/tmp/toolchain.cmake \
      -DCMAKE_REQUIRED_DEFINITIONS=-D_GNU_SOURCE -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_INSTALL_PREFIX="$ROOT/usr" -DCMAKE_INSTALL_LIBDIR=lib/swift_static/linux-static \
      -DBUILD_SHARED_LIBS=NO -DBUILD_FULLY_STATIC=YES -DSWIFT_SYSTEM_NAME=linux-static \
      -DFOUNDATION_PATH_TO_LIBDISPATCH_SOURCE="$DSRC" -DFOUNDATION_PATH_TO_LIBDISPATCH_BUILD="$B/dispatch" \
      -D_SwiftFoundation_SourceDIR="$DEPS/swift-foundation" -D_SwiftFoundationICU_SourceDIR="$DEPS/swift-foundation-icu" \
      -D_SwiftCollections_SourceDIR="$DEPS/swift-collections" \
      -DSwiftFoundation_MACRO=/usr/lib/swift/host/plugins/libFoundationMacros.so \
      -DCMAKE_Swift_COMPILER_WORKS=YES -Ddispatch_DIR="$B/dispatch/cmake/modules" >/dev/null
cmake --build "$B/foundation" --target Foundation --parallel "$(nproc)"

da=$(find "$B/dispatch" -name libdispatch.a -type f | head -1)
fa=$(find "$B/foundation" -name libFoundation.a -type f | head -1)
[ -n "$da" ] && [ -n "$fa" ] || { echo "::error::build produced no archive"; exit 1; }

mkdir -p "$OUT/$ARCH" "$OUT/checks"
syms() { nm -g --defined-only "$@" 2>/dev/null | awk 'NF>=3 && $2 ~ /^[TDBRWVuvi]$/ {print $2, $3}' | sort -u; }
fail=0
for pair in "libdispatch.a:$SDK/official-libdispatch.a:$da" "libFoundation.a:$SDK/official-libFoundation.a:$fa"; do
  IFS=: read -r name off new <<<"$pair"
  syms "$off" > "$OUT/checks/$ARCH-$name-official.txt"
  syms "$new" > "$OUT/checks/$ARCH-$name-built.txt"
  if ! diff -u "$OUT/checks/$ARCH-$name-official.txt" "$OUT/checks/$ARCH-$name-built.txt"; then
    echo "::error::$ARCH $name global symbol set differs from the official bundle"; fail=1
  fi
  install -m 0644 "$new" "$OUT/$ARCH/$name"
done
[ "$fail" = 0 ] || exit 1
{
  echo "component: static-sdk $TRIPLE (libdispatch.a, libFoundation.a)"
  echo "libdispatch source: $(git -c safe.directory="*" -C "$DSRC" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "foundation source: $(git -c safe.directory="*" -C "$FSRC" rev-parse HEAD 2>/dev/null || echo unknown)"
  for d in swift-foundation swift-foundation-icu swift-collections; do echo "dep $d: $(git -c safe.directory="*" -C "$DEPS/$d" rev-parse HEAD 2>/dev/null || echo unknown)"; done
  echo "host: $(uname -m); $(clang --version | head -1); $(swiftc --version 2>&1 | head -1)"
  echo "libdispatch.a symbols: $(wc -l < "$OUT/checks/$ARCH-libdispatch.a-built.txt") (identical to official)"
  echo "libFoundation.a symbols: $(wc -l < "$OUT/checks/$ARCH-libFoundation.a-built.txt") (identical to official)"
  (cd "$OUT/$ARCH" && sha256sum libdispatch.a libFoundation.a)
} > "$OUT/MANIFEST-static-sdk-$ARCH.txt"
cat "$OUT/MANIFEST-static-sdk-$ARCH.txt"
