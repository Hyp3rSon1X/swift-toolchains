#!/bin/bash
# swift-6.4.2-RELEASE wasm32-unknown-wasip1 + wasip1-threads stdlib packages and Swift SDK. Run inside build-slot.
# Usage: build.sh [--dry-run]
set -uo pipefail
W=${W:?set W to the build directory}
export TOOLCHAIN_VERSION=swift-6.4.2-RELEASE
export SWIFT_BUILD_ROOT=$W/build
export PATH=/opt/homebrew/bin:$PATH
cd $W/src
S=$(date +%s)
swift/utils/build-script --release --no-assertions --no-swift-stdlib-assertions \
  --build-subdir=wasm \
  --llvm-targets-to-build="AArch64;WebAssembly" \
  --build-wasi-stdlib \
  --host-target macosx-arm64 --stdlib-deployment-targets=macosx-arm64 \
  --swift-darwin-supported-archs arm64 --skip-ios --skip-tvos --skip-watchos --skip-xros \
  --skip-build-benchmarks --skip-test-wasi-stdlib --skip-test-swift --skip-test-cmark --skip-test-libdispatch --skip-test-foundation --skip-test-xctest \
  --skip-early-swift-driver --no-llvm-include-tests --build-embedded-stdlib=0 --build-embedded-stdlib-cross-compiling=0 \
  --skip-build-lldb --swift-driver --swiftsyntax --install-llvm --llvm-install-components="llvm-ar;llvm-ranlib;llvm-nm;llvm-objdump;clang;clang-resource-headers;builtins;runtimes;clang-features-file;lld" --install-swift --install-swiftsyntax --install-swift-driver \
  --extra-cmake-options="-DSWIFT_TOOLCHAIN_VERSION=swift-6.4.2-RELEASE -DSWIFT_APPEND_VC_REV=OFF" \
  --jobs "${BUILD_SLOT_JOBS:-4}" "$@"
echo "build-script exit $? after $(( $(date +%s) - S ))s"
