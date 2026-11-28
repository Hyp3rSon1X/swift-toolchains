#!/bin/bash
# Run after ../fetch-src.sh: switches $W/src/swift and $W/src/llvm-project to the fork commits that carry the tail-call
# picks and the Mutex fix, and $W/src/wasi-libc to the fork commit that exports __stack_limit (branch hyp3r/wasm-sdk/6.4.2
# in Hyp3rSon1X/swift, Hyp3rSon1X/llvm-project and Hyp3rSon1X/wasi-libc), then applies build-system.patch to swift.
# Usage: W=<build dir> use-forks.sh
set -euo pipefail
W=${W:?set W to the build directory}
HERE=$(cd "$(dirname "$0")" && pwd)
SWIFT=fd5fcb133ad28ba1ac3036cae9a5afd9a120c753
LLVM=11627bca6d44ea33b0fbb74d4c5c0e6c45acc087
WASI_LIBC=85613c6915f511541354e6f285e6d6a59900581d
sw() { # repo-dir fork-url commit
  git -C "$W/src/$1" fetch --quiet "$2" "$3" 2>/dev/null || git -C "$W/src/$1" fetch --quiet "$2" hyp3r/wasm-sdk/6.4.2
  git -C "$W/src/$1" checkout --quiet --detach "$3"
  test "$(git -C "$W/src/$1" rev-parse HEAD)" = "$3"
}
sw swift https://github.com/Hyp3rSon1X/swift.git $SWIFT
sw llvm-project https://github.com/Hyp3rSon1X/llvm-project.git $LLVM
sw wasi-libc https://github.com/Hyp3rSon1X/wasi-libc.git $WASI_LIBC
git -C "$W/src/swift" apply "$HERE/build-system.patch"
echo "swift $(git -C "$W/src/swift" rev-parse --short=12 HEAD) + build-system.patch, llvm-project $(git -C "$W/src/llvm-project" rev-parse --short=12 HEAD), wasi-libc $(git -C "$W/src/wasi-libc" rev-parse --short=12 HEAD)"
