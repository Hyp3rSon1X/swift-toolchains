#!/bin/bash
# Build recipe from SwixForge's swix/6.4.0 branch of the SwiftPM fork.
# Lays out the build workspace the official Linux toolchain build uses: SwiftPM in
# /home/build-user/swiftpm and every dependency in a sibling directory, at the revisions in
# overlays/swiftpm/deps.txt. Runs inside the pinned swift:6.4.0-noble image.
#
# Usage: overlays/swiftpm/workspace.sh <swiftpm source dir>
# Installs the build prerequisites the image lacks (python3 for Utilities/bootstrap, curl, file,
# libsqlite3-dev and libncurses-dev for the system libraries SwiftPM and llbuild link).
set -euo pipefail
src=$(realpath "$1")
here=$(cd "$(dirname "$0")" && pwd)
workspace=/home/build-user

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq > /dev/null
apt-get install -y -qq --no-install-recommends python3 libsqlite3-dev libncurses-dev git ca-certificates curl file > /dev/null

git config --global --add safe.directory '*'
mkdir -p "$workspace"
rm -rf "$workspace/swiftpm"
cp -a "$src" "$workspace/swiftpm"
rm -rf "$workspace/swiftpm/.build"

grep -v '^#' "$here/deps.txt" | while read -r dir url tag commit; do
    [[ -z "$dir" ]] && continue
    if [[ "$(git -C "$workspace/$dir" rev-parse HEAD 2>/dev/null)" != "$commit" ]]; then
        rm -rf "$workspace/$dir"
        git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" "$url" "$workspace/$dir"
    fi
    actual=$(git -C "$workspace/$dir" rev-parse HEAD)
    if [[ "$actual" != "$commit" ]]; then
        echo "FAIL: $dir is $actual, expected $commit ($tag)" >&2
        exit 1
    fi
    echo "$dir $tag $commit"
done
