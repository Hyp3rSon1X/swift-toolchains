#!/bin/bash
# Packages the two variants built by build.sh into one deterministic zip each, plus SHA256SUMS.
# Usage: OUT=<build.sh output dir> SDK_ZIP=<swift-6.4.2-RELEASE_wasm-threads-tailcall.artifactbundle.zip> [DIST=<dir>] package.sh
# Layout of each zip (one top-level directory):
#   PackageDescription.swiftmodule/  CompilerPluginSupport.swiftmodule/   swiftmodule, swiftdoc, swiftinterface, private.swiftinterface
#   lib/libPackageDescription.a  lib/libCompilerPluginSupport.a  lib/link-libs.txt
#   LICENSE.txt  NOTICE.txt  provenance.json  (essentials also: the patch)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "${OUT:?set OUT}" && pwd)
SDK_ZIP=${SDK_ZIP:?set SDK_ZIP}
DIST=${DIST:-$OUT/dist}
RELEASE=swifthost-packagedescription-6.4.2-1
PATCH=$HERE/swiftpm-wasi-foundation-essentials.patch
SDK_ID=swift-6.4.2-RELEASE_wasm-threads-tailcall
read -r SWIFTPM_TAG SWIFTPM_COMMIT < "$OUT/swiftpm-version.txt"
SDK_SHA=$(shasum -a 256 "$SDK_ZIP" | cut -d' ' -f1)
PATCH_SHA=$(shasum -a 256 "$PATCH" | cut -d' ' -f1)
mkdir -p "$DIST"; DIST=$(cd "$DIST" && pwd)
STAGE=$OUT/stage; rm -rf "$STAGE"; mkdir -p "$STAGE"
rm -f "$DIST"/"$RELEASE"-*.zip "$DIST"/SHA256SUMS "$DIST"/*.patch

for variant in essentials foundation; do
  name=$RELEASE-$variant
  d=$STAGE/$name; mkdir -p "$d/lib"
  cp -R "$OUT/variants/$variant/PackageDescription.swiftmodule" "$OUT/variants/$variant/CompilerPluginSupport.swiftmodule" "$d/"
  cp "$OUT/variants/$variant/lib/"* "$d/lib/"
  cp "$OUT/src-foundation/LICENSE.txt" "$OUT/src-foundation/NOTICE.txt" "$d/"
  patch_sha=none
  if [ $variant = essentials ]; then cp "$PATCH" "$d/"; patch_sha=$PATCH_SHA; fi
  python3 -I - "$d/provenance.json" "$variant" "$SWIFTPM_TAG" "$SWIFTPM_COMMIT" "$SDK_ID" "$SDK_SHA" "$patch_sha" \
    "$OUT/flags.txt" "$OUT/compiler.txt" "$RELEASE" <<'PY'
import json, sys
out, variant, tag, commit, sdk_id, sdk_sha, patch_sha, flags, compiler, release = sys.argv[1:]
lines = lambda p: [l for l in open(p).read().splitlines() if l]
doc = {
    "release": release,
    "variant": variant,
    "description": "SwiftPM PackageDescription and CompilerPluginSupport (the manifest runtime) as static libraries",
    "target": "wasm32-unknown-wasip1-threads",
    "tail_calls": True,
    "imports": "FoundationEssentials" if variant == "essentials" else "Foundation",
    "source": {
        "repository": "https://github.com/swiftlang/swift-package-manager",
        "tag": tag,
        "commit": commit,
        "paths": ["Sources/Runtimes/PackageDescription", "Sources/Runtimes/CompilerPluginSupport"],
        "patch": None if patch_sha == "none" else {"file": "swiftpm-wasi-foundation-essentials.patch", "sha256": patch_sha},
    },
    # The compiler that built the SDK (see the SDK recipe's README for the cherry-picks on top of swift-6.4.2-RELEASE).
    "compiler": {
        "version": lines(compiler)[0],
        "swift": {"repository": "https://github.com/Hyp3rSon1X/swift", "branch": "hyp3r/wasm-sdk/6.4.2",
                  "commit": "a9c0b4f09a79885037a86d377ccb470efee6a939"},
        "llvm-project": {"repository": "https://github.com/Hyp3rSon1X/llvm-project", "branch": "hyp3r/wasm-sdk/6.4.2",
                         "commit": "11627bca6d44ea33b0fbb74d4c5c0e6c45acc087"},
    },
    "sdk": {"id": sdk_id, "artifact_bundle_zip_sha256": sdk_sha},
    "flags": lines(flags),
    "link_libs_file": "lib/link-libs.txt",
}
open(out, "w").write(json.dumps(doc, indent=2, sort_keys=True) + "\n")
PY
  # Deterministic zip: fixed mtimes, sorted entries, no extra fields.
  find "$d" -exec touch -h -t 202611200000 {} +
  (cd "$STAGE" && find "$name" | LC_ALL=C sort | zip -X -q -@ "$DIST/$name.zip")
done
cp "$PATCH" "$DIST/"
(cd "$DIST" && shasum -a 256 *.zip *.patch > SHA256SUMS && cat SHA256SUMS)
