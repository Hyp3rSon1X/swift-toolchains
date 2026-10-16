#!/bin/bash
# Usage: swiftpm-prebuilts.sh <image> <arch> <spec> <source-tag> <outdir>
#   image       a built full image whose compiler decides the prebuilt name (docker run <image> swiftc -print-target-info)
#   arch        x86_64 | aarch64
#   spec        <package>:<version>, e.g. swift-syntax:604.0.0
#   source-tag  which official swift.org files to fetch, e.g. swift-6.4.0-RELEASE
#   outdir      receives <package>/<version>/<name>.json and <name>-MacroSupport.tar.gz, <name> = <compilerTag>-ubuntu_noble_<arch>
# SwiftPM 6.4 looks the files up under that name; <compilerTag> is .swiftCompilerTag of the target info, else the
# `(swift-...)` part of .compilerVersion. Our Linux frontend has no swiftCompilerTag, so the tag comes from the version
# string. The signed manifest has no version inside, so the official file stays valid under another name. SwiftPM
# checks the archive's sha256 only for https downloads, not for file:// URLs, so we verify it against the manifest here,
# before anything is baked into an image (and tests/prebuilts-swift-syntax re-checks it in the built image).
# DOCKER overrides the container CLI (e.g. DOCKER="container" for Apple's container tool).
set -euo pipefail
image=$1; arch=$2; spec=$3; srctag=$4; out=$5
pkg=${spec%%:*}; ver=${spec#*:}
docker=${DOCKER:-docker}
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
info=$($docker run --rm "$image" swiftc -print-target-info)
ctag=$(jq -r '.swiftCompilerTag // empty' <<<"$info")
[ -n "$ctag" ] || ctag=$(jq -r '.compilerVersion' <<<"$info" | sed -nE 's/.*\((swift(lang)?-[^ )]*).*/\1/p' | head -1)
[ -n "$ctag" ] || { echo "::error::no compiler tag in target info"; echo "$info" | head -20; exit 1; }
name="${ctag}-ubuntu_noble_${arch}"
src="${srctag}-ubuntu_noble_${arch}"
base="https://download.swift.org/prebuilts/${pkg}/${ver}"
d="$out/$pkg/$ver"; mkdir -p "$d"
curl -fsSL --retry 3 -o "$d/$name.json" "$base/$src.json"
curl -fsSL --retry 3 -o "$d/$name-MacroSupport.tar.gz" "$base/$src-MacroSupport.tar.gz"
want=$(jq -r '.manifest.libraries[] | select(.name=="MacroSupport") | .checksum' "$d/$name.json")
got=$(sha "$d/$name-MacroSupport.tar.gz")
[ "$want" = "$got" ] || { echo "::error::MacroSupport sha256 $got != manifest $want"; exit 1; }
echo "prebuilt name: $name (compiler tag $ctag, official files $src)"
echo "official sha256 manifest-json $(sha "$d/$name.json")"
echo "official sha256 MacroSupport $got"
