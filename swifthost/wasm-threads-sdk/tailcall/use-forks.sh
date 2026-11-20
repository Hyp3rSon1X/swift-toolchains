#!/bin/bash
# Run after ../fetch-src.sh: switches $W/src/swift and $W/src/llvm-project to the fork commits that carry the tail-call
# picks and the Mutex fix (branch hyp3r/wasm-sdk/6.4.2 in Hyp3rSon1X/swift and Hyp3rSon1X/llvm-project), then applies build-system.patch
# to swift. Usage: W=<build dir> use-forks.sh
set -euo pipefail
W=${W:?set W to the build directory}
HERE=$(cd "$(dirname "$0")" && pwd)
SWIFT=37591f512993731dc2a9739dfb2912fdf5d17775
LLVM=11627bca6d44ea33b0fbb74d4c5c0e6c45acc087
sw() { # repo-dir fork-url commit
  git -C "$W/src/$1" fetch --quiet "$2" "$3" 2>/dev/null || git -C "$W/src/$1" fetch --quiet "$2" hyp3r/wasm-sdk/6.4.2
  git -C "$W/src/$1" checkout --quiet --detach "$3"
  test "$(git -C "$W/src/$1" rev-parse HEAD)" = "$3"
}
sw swift https://github.com/Hyp3rSon1X/swift.git $SWIFT
sw llvm-project https://github.com/Hyp3rSon1X/llvm-project.git $LLVM
git -C "$W/src/swift" apply "$HERE/build-system.patch"
echo "swift $(git -C "$W/src/swift" rev-parse --short=12 HEAD) + build-system.patch, llvm-project $(git -C "$W/src/llvm-project" rev-parse --short=12 HEAD)"
