#!/bin/bash
# Fetches the swift-6.4.2-RELEASE sources (release/6.4.2 update-checkout scheme pins) into $W/src.
# Usage: W=<build dir> [LOCAL_CHECKOUT=<update-checkout tree>] fetch-src.sh
# With LOCAL_CHECKOUT, the large repositories become worktrees of that tree's clones (which must have the tag);
# otherwise everything is shallow-cloned from GitHub. Both give the same commits.
set -u
W=${W:?set W to the build directory}
T=swift-6.4.2-RELEASE
mkdir -p "$W/src"; cd "$W/src"
cl() { # name url ref
  [ -e "$1" ] || git clone --quiet --depth 1 --branch "$3" "$2" "$1"; }
for r in llvm-project swift cmark swift-syntax swift-driver swift-experimental-string-processing; do
  if [ -n "${LOCAL_CHECKOUT:-}" ]; then [ -e "$r" ] || git -C "$LOCAL_CHECKOUT/$r" worktree add --detach "$W/src/$r" $T
  else
    case $r in cmark) u=swift-cmark;; *) u=$r;; esac
    cl $r https://github.com/swiftlang/$u.git $T
  fi
done
for r in swift-llvm-bindings llbuild swift-tools-support-core swift-testing swift-corelibs-xctest swift-corelibs-foundation swift-foundation-icu swift-foundation swift-corelibs-libdispatch swift-corelibs-blocksruntime swift-sdk-generator swiftpm swift-build swift-tools-protocols; do
  case $r in llbuild) u=swift-llbuild;; swiftpm) u=swift-package-manager;; *) u=$r;; esac
  cl $r https://github.com/swiftlang/$u.git $T
done
cl wasi-libc https://github.com/WebAssembly/wasi-libc.git wasi-sdk-31
cl wasmkit https://github.com/swiftwasm/WasmKit.git 0.3.1
cl libxml2 https://github.com/GNOME/libxml2.git v2.11.5
cl swift-argument-parser https://github.com/apple/swift-argument-parser.git 1.6.1
cl swift-atomics https://github.com/apple/swift-atomics.git 1.2.0
cl swift-collections https://github.com/apple/swift-collections.git 1.1.6
cl swift-crypto https://github.com/apple/swift-crypto.git 3.12.5
cl swift-certificates https://github.com/apple/swift-certificates.git 1.10.1
cl swift-asn1 https://github.com/apple/swift-asn1.git 1.3.2
cl swift-async-algorithms https://github.com/apple/swift-async-algorithms.git 1.0.1
cl swift-log https://github.com/apple/swift-log.git 1.5.4
cl swift-numerics https://github.com/apple/swift-numerics.git 1.0.2
cl swift-system https://github.com/apple/swift-system.git 1.5.0
cl swift-toolchain-sqlite https://github.com/swiftlang/swift-toolchain-sqlite.git 1.0.9
cl swift-subprocess https://github.com/swiftlang/swift-subprocess.git 0.4
cl swift-nio https://github.com/apple/swift-nio.git 2.92.2
cl zlib https://github.com/madler/zlib.git v1.3.1
echo FETCHDONE
