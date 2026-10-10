#!/bin/bash
# Usage: fetch.sh <workspace> <swift-repo> <swift-ref> <upstream-tag>
# Shallow checkouts of the trees swift-frontend is built from: the patched swift tree at <swift-ref> and the
# unmodified <upstream-tag> revisions of everything else (the release tags are the same name in every repo).
set -euo pipefail
WS="$1"; SWIFT_REPO="$2"; SWIFT_REF="$3"; TAG="$4"
mkdir -p "$WS"; cd "$WS"
clone() { git clone -q --depth 1 --branch "$TAG" "https://github.com/$1.git" "$2" 2>&1 | grep -v '^Note: switching\|^You are in\|^changes\|^state\|^do so\|^$\|^ *git switch\|^Or undo\|^Turn off\|^If you want' || true; }
clone swiftlang/llvm-project llvm-project &
clone swiftlang/swift-cmark cmark &
clone swiftlang/swift-syntax swift-syntax &
clone swiftlang/swift-experimental-string-processing swift-experimental-string-processing &
mkdir swift && git -C swift init -q && git -C swift fetch -q --depth 1 "https://github.com/$SWIFT_REPO.git" "$SWIFT_REF" && git -C swift checkout -q FETCH_HEAD &
wait
for d in swift llvm-project cmark swift-syntax swift-experimental-string-processing; do
  [ -e "$d/.git" ] || { echo "::error::checkout of $d failed"; exit 1; }
  echo "$d $(git -C "$d" rev-parse HEAD)"
done
