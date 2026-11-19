#!/bin/bash
# Builds the combined wasm macro plugin. Usage: build.sh [WORKDIR]   (default: ./build)
# Needs: src/{swift-syntax,swift-testing,swift-foundation,swift} cloned at swift-6.4.2-RELEASE
# (swift: sparse lib/Macros) and the swift.org wasm SDK bundle. WRAP is an optional command prefix for the
# heavy build step (e.g. a job scheduler); it is empty by default.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-$PWD/build}
SDKBUNDLE=${SDKBUNDLE:?set SDKBUNDLE to the unpacked swift-6.4.2-RELEASE_wasm.artifactbundle}
TC=${TC:-$HOME/Library/Developer/Toolchains/swift-6.4.2-hyp3r.9.xctoolchain/usr/bin}
PKG=$W/pkg
mkdir -p "$PKG/Sources" "$W/sdks" "$W/dist"
cp "$HERE/Package.swift" "$PKG/Package.swift"
rm -rf "$PKG/Sources/BuiltinMacrosPlugin"; cp -R "$HERE/Sources/BuiltinMacrosPlugin" "$PKG/Sources/"
ln -sfn "$W/src/swift/lib/Macros/Sources/ObservationMacros" "$PKG/Sources/ObservationMacros"
ln -sfn "$W/src/swift/lib/Macros/Sources/SwiftMacros" "$PKG/Sources/SwiftMacros"
ln -sfn "$W/src/swift-testing/Sources/TestingMacros" "$PKG/Sources/TestingMacros"
ln -sfn "$W/src/swift-foundation/Sources/FoundationMacros" "$PKG/Sources/FoundationMacros"
ln -sfn "$SDKBUNDLE" "$W/sdks/$(basename "$SDKBUNDLE")"
cd "$PKG"
SWIFT_SYNTAX_PATH="$W/src/swift-syntax" PATH="$TC:$PATH" ${WRAP:-} "$TC/swift" build \
  --swift-sdks-path "$W/sdks" --swift-sdk swift-6.4.2-RELEASE_wasm \
  -c release --product swift-builtin-macros \
  -Xlinker --strip-all
OUT=$PKG/.build/out/Products/Release-webassembly-wasm32/swift-builtin-macros.wasm # swiftbuild backend layout
cp "$OUT" "$W/dist/swift-builtin-macros-6.4.2-wasm32-wasip1.wasm"
ls -l "$W/dist"
