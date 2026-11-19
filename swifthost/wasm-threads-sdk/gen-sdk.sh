#!/bin/bash
# Packages one target with swift-sdk-generator (6.4.2 tag) the way wasiswiftsdk.py does, using the host Xcode swift
# (the just-built toolchain has no swift-run since swiftpm is not built). Usage: gen-sdk.sh wasm32-unknown-wasip1[-threads]
set -euo pipefail
W=${W:?set W to the build directory}
T=$1
M=${T#wasm32-unknown-}; M=wasm32-$M
export SWIFTCI_USE_LOCAL_DEPS=1 TOOLCHAIN_VERSION=swift-6.4.2-RELEASE
mkdir -p "$W/dist/gen"; cd "$W/dist/gen"
xcrun swift run --package-path $W/src/swift-sdk-generator --build-path $W/build/wasm/wasiswiftsdk-macosx-arm64 \
  swift-sdk-generator make-wasm-sdk --swift-version swift-6.4.2-RELEASE --target $T \
  --target-swift-package-path $W/build/wasm/wasiswiftsdk-macosx-arm64/Toolchains/$T \
  --wasi-sysroot $W/build/wasm/wasi-sysroot/$M/sysroot
