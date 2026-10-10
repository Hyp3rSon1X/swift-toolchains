#!/bin/bash
# Runs inside the official Swift image. Usage: build-repros.sh <official-bundle.tar.gz> <patched-bundle.tar.gz> <arch> <out-dir>
# Installs both Static Linux SDK bundles side by side and cross-compiles the SC-0001 (libdispatch epoll) and SC-0002
# (Foundation Process) reproducers statically against each, for <arch>-swift-linux-musl. The binaries are plain
# static executables; run-repros.sh runs them anywhere (no Swift needed).
set -euo pipefail
OFFICIAL="$1"; PATCHED="$2"; ARCH="$3"; OUT="$4"
here="$(cd "$(dirname "$0")" && pwd)"; tests="$(dirname "$here")"
mkdir -p "$OUT"

install_sdk() {  # install_sdk <tarball>: prints the SDK id
  local f="$1" sum
  sum=$(swift package compute-checksum "$f")
  swift sdk install "$f" --checksum "$sum" >&2
  tar tzf "$f" | awk -F/ 'NR==1 {sub(/\.artifactbundle$/, "", $1); print $1}'
}
off_id=$(install_sdk "$OFFICIAL"); pat_id=$(install_sdk "$PATCHED")
echo "official id: $off_id"; echo "patched id:  $pat_id"
swift sdk list

build() {  # build <tag> <sdk-id> <test dir> <name>
  local tag="$1" id="$2" dir="$3" name="$4" p="/tmp/pkg-$1-$4"
  rm -rf "$p"; mkdir -p "$p/Sources/repro"; cp "$tests/$dir/main.swift" "$p/Sources/repro/main.swift"
  cat > "$p/Package.swift" <<PKG
// swift-tools-version:6.0
import PackageDescription
let package = Package(name: "repro", targets: [.executableTarget(name: "repro", swiftSettings: [.swiftLanguageMode(.v5)])])
PKG
  (cd "$p" && swift build -c release --swift-sdk "$id" --triple "$ARCH-swift-linux-musl" >/tmp/build-$tag-$name.log 2>&1) || { tail -30 "/tmp/build-$tag-$name.log"; exit 1; }
  bin=$(cd "$p" && swift build -c release --swift-sdk "$id" --triple "$ARCH-swift-linux-musl" --show-bin-path)
  cp "$bin/repro" "$OUT/$name-$tag"
  file "$OUT/$name-$tag" 2>/dev/null || true
}
for tag in official patched; do
  id=$off_id; [ $tag = patched ] && id=$pat_id
  build "$tag" "$id" SC-0001-libdispatch-epollhup sc0001
  build "$tag" "$id" SC-0002-foundation-process sc0002
done
ls -la "$OUT"
