#!/bin/bash
# Builds SwiftPM's PackageDescription and CompilerPluginSupport (the manifest runtime) for wasm32-unknown-wasip1-threads
# with WebAssembly tail calls, in two variants:
#   essentials  imports FoundationEssentials only (swiftpm-wasi-foundation-essentials.patch applied); no ICU
#   foundation  unpatched sources, imports Foundation
#
# Usage: SDK_BUNDLE=<dir> SWIFTC=<path> TOOLS=<dir> [SWIFTPM_REPO=<url or path>] [OUT=<dir>] build.sh
#   SDK_BUNDLE  the unpacked swift-6.4.2-RELEASE_wasm-threads-tailcall.artifactbundle directory
#   SWIFTC      the swiftc that built that SDK (Swift 6.4.2 with the tail-call patches)
#   TOOLS       a directory with wasm-ld and llvm-ar (put on PATH; swiftc links through them)
#   OUT         build directory, default ./out; the results are in $OUT/variants/<variant>
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SDK_BUNDLE=${SDK_BUNDLE:?set SDK_BUNDLE}
SWIFTC=${SWIFTC:?set SWIFTC}
TOOLS=${TOOLS:?set TOOLS}
SWIFTPM_REPO=${SWIFTPM_REPO:-https://github.com/swiftlang/swift-package-manager.git}
OUT=${OUT:-$PWD/out}

SWIFTPM_TAG=swift-6.4.2-RELEASE
SWIFTPM_COMMIT=8054c59f07a9c8cc8e67b3ec57870710c7154854
SDK_ID=swift-6.4.2-RELEASE_wasm-threads-tailcall
PATCH=$HERE/swiftpm-wasi-foundation-essentials.patch
TRIPLE=wasm32-unknown-wasip1-threads

B=$SDK_BUNDLE/$SDK_ID/$TRIPLE
SR=$B/swift.xctoolchain/usr/lib/swift_static
[ -d "$SR" ] || { echo "no SDK at $B" >&2; exit 1; }
export PATH=$TOOLS:$PATH
AUTOLINK_EXTRACT=$(dirname "$SWIFTC")/swift-autolink-extract

# Everything Swift code needs for this SDK: -Xcc -mtail-call is required, see the SDK's README.
COMMON=(-target $TRIPLE -sdk "$B/WASI.sdk" -resource-dir "$SR" -static-stdlib
  -Xclang-linker -resource-dir -Xclang-linker "$SR/clang"
  -Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call)
# SwiftPM's own flags (Sources/Runtimes/CMakeLists.txt) plus -O. ConciseMagicFile makes #file expand to
# "PackageDescription/PackageDependency.swift" instead of the absolute path of the build directory, so the archives
# do not depend on where they were built.
LIBFLAGS=(-O -wmo -parse-as-library -swift-version 5 -enable-library-evolution -package-description-version 999.0
  -enable-upcoming-feature ConciseMagicFile)

mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

# Sources at the pinned commit.
if [ ! -d "$OUT/src-foundation/.git" ]; then
  git clone -q --no-checkout "$SWIFTPM_REPO" "$OUT/src-foundation"
  git -C "$OUT/src-foundation" checkout -q --detach "$SWIFTPM_COMMIT"
fi
[ "$(git -C "$OUT/src-foundation" rev-parse HEAD)" = "$SWIFTPM_COMMIT" ] || { echo "wrong SwiftPM commit" >&2; exit 1; }
if [ ! -f "$OUT/src-essentials/.patched" ]; then
  rm -rf "$OUT/src-essentials"; git clone -q --no-checkout "$OUT/src-foundation" "$OUT/src-essentials"
  git -C "$OUT/src-essentials" checkout -q --detach "$SWIFTPM_COMMIT"
  git -C "$OUT/src-essentials" apply "$PATCH"
  touch "$OUT/src-essentials/.patched"
fi

build_variant() { # <variant> <source dir>
  local variant=$1 src=$2/Sources/Runtimes dest=$OUT/variants/$1 obj=$OUT/obj/$1
  rm -rf "$dest" "$obj"; mkdir -p "$dest/lib" "$dest/PackageDescription.swiftmodule" "$dest/CompilerPluginSupport.swiftmodule" "$obj"
  local files=()
  while read -r f; do files+=("$src/PackageDescription/$f"); done < <(sed -n 's/^  \([A-Za-z+]*\.swift\))\{0,1\}$/\1/p' "$src/PackageDescription/CMakeLists.txt")
  [ ${#files[@]} = 18 ] || { echo "expected 18 PackageDescription sources, got ${#files[@]}" >&2; exit 1; }
  local cache=${MODULE_CACHE:-$obj/modulecache}; rm -rf "$cache"
  "$SWIFTC" "${COMMON[@]}" "${LIBFLAGS[@]}" -D USE_IMPL_ONLY_IMPORTS -module-name PackageDescription \
    -module-cache-path "$cache" -Xfrontend -Rmodule-interface-rebuild \
    -emit-module -emit-module-path "$dest/PackageDescription.swiftmodule/$TRIPLE.swiftmodule" \
    -emit-module-interface-path "$dest/PackageDescription.swiftmodule/$TRIPLE.swiftinterface" \
    -emit-private-module-interface-path "$dest/PackageDescription.swiftmodule/$TRIPLE.private.swiftinterface" \
    -c -o "$obj/PackageDescription.o" "${files[@]}" > "$obj/PackageDescription.log" 2>&1 || { cat "$obj/PackageDescription.log"; exit 1; }
  "$SWIFTC" "${COMMON[@]}" "${LIBFLAGS[@]}" -module-name CompilerPluginSupport \
    -module-cache-path "$cache" -Xfrontend -Rmodule-interface-rebuild -I "$dest" \
    -emit-module -emit-module-path "$dest/CompilerPluginSupport.swiftmodule/$TRIPLE.swiftmodule" \
    -emit-module-interface-path "$dest/CompilerPluginSupport.swiftmodule/$TRIPLE.swiftinterface" \
    -emit-private-module-interface-path "$dest/CompilerPluginSupport.swiftmodule/$TRIPLE.private.swiftinterface" \
    -c -o "$obj/CompilerPluginSupport.o" "$src/CompilerPluginSupport/TargetExtensions.swift" \
    > "$obj/CompilerPluginSupport.log" 2>&1 || { cat "$obj/CompilerPluginSupport.log"; exit 1; }
  # swiftsourceinfo holds absolute paths of the sources; nothing here needs it.
  rm -f "$dest"/*.swiftmodule/*.swiftsourceinfo
  # Deterministic archives (D: zero timestamps, uid and gid).
  llvm-ar rcsD "$dest/lib/libPackageDescription.a" "$obj/PackageDescription.o"
  llvm-ar rcsD "$dest/lib/libCompilerPluginSupport.a" "$obj/CompilerPluginSupport.o"
  # The libraries import Foundation only implementation-only, so the module files do not carry the link libraries.
  # The autolink entries in the objects do; keep them next to the archives for the final link.
  "$AUTOLINK_EXTRACT" "$obj/PackageDescription.o" "$obj/CompilerPluginSupport.o" -o - | grep -v '^$' | awk '!seen[$0]++' \
    > "$dest/lib/link-libs.txt"
  echo "built $variant: $(ls "$dest/lib" | tr '\n' ' ')"
}

build_variant essentials "$OUT/src-essentials"
build_variant foundation "$OUT/src-foundation"
# What package.sh records in provenance.json: the flags (SDK path abbreviated), the compiler version, the sources.
{ printf '%s\n' "${COMMON[@]}" "${LIBFLAGS[@]}" | sed "s#$B#<sdk>#g"; } > "$OUT/flags.txt"
"$SWIFTC" --version 2>&1 | grep -m1 '^Swift version' > "$OUT/compiler.txt"
echo "$SWIFTPM_TAG $SWIFTPM_COMMIT" > "$OUT/swiftpm-version.txt"
