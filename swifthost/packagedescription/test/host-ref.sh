#!/bin/bash
# Reference run: compile every manifest with a host toolchain against its own PackageDescription (the way SwiftPM does,
# see ManifestLoader.swift) and run it natively with `-fileno 1`.
# Usage: host-ref.sh <toolchain usr dir> <out dir>
set -u
. "$(dirname "$0")/common.sh"
TC=${1:?toolchain usr dir}; OUT=${2:?out dir}
API=$TC/lib/swift/pm/ManifestAPI
SDKROOT_PATH=$(xcrun --sdk macosx --show-sdk-path)
mkdir -p "$OUT"
for m in "$MANIFESTS"/*.swift; do
  n=$(basename "$m" .swift); tv=$(tools_version "$m"); sv=$(swift_language_version "$tv")
  D=$OUT/$n; rm -rf "$D"; mkdir -p "$D"; stage_manifest "$m" "$D"
  ( cd "$D" && "$TC/bin/swiftc" -module-name main -swift-version "$sv" -I "$API" -L "$API" -lPackageDescription \
      -Xlinker -rpath -Xlinker "$API" -sdk "$SDKROOT_PATH" -package-description-version "$tv" \
      Package.swift -o manifest > compile.log 2>&1 ); crc=$?
  if [ $crc = 0 ]; then ( cd "$D" && ./manifest -context "$CONTEXT_JSON" -fileno 1 > manifest.json 2> run.err ); rrc=$?; else rrc=-; fi
  echo "$n tools=$tv compile=$crc run=$rrc"
done
