#!/bin/bash
# Turns the generator's threads bundle into the release zip.
# Usage: W=<build dir> package.sh   (after build.sh and gen-sdk.sh wasm32-unknown-wasip1-threads)
# The only change to the generator output: the Embedded Swift variant is removed. Upstream 6.4.2 builds no Embedded
# stdlib for wasm32-unknown-wasip1-threads, so that variant failed with "could not find module 'Swift' for target
# 'wasm32-unknown-wasip1-threads'".
set -euo pipefail
W=${W:?set W to the build directory}
ID=swift-6.4.2-RELEASE_wasm-threads
GEN=${GEN:-$W/src/swift-sdk-generator/Bundles/$ID.artifactbundle}   # where swift-sdk-generator writes it
OUT=$W/dist/final
rm -rf "$OUT"; mkdir -p "$OUT"
cp -R "$GEN" "$OUT/"
B=$OUT/$ID.artifactbundle
T=$B/$ID/wasm32-unknown-wasip1-threads
python3 -I -B - "$B/info.json" "$ID-embedded" <<'PY'
import json, sys, collections
path, drop = sys.argv[1], sys.argv[2]
d = json.load(open(path), object_pairs_hook=collections.OrderedDict)
del d['artifacts'][drop]
open(path, 'w').write(json.dumps(d, indent=2, separators=(',', ' : ')))
PY
rm -rf "$T/swift.xctoolchain/usr/lib/swift/embedded" "$T/swift.xctoolchain/usr/lib/swift_static/embedded" \
  "$T/embedded-swift-sdk.json" "$T/embedded-toolset.json"
# Deterministic zip: fixed mtimes, sorted entries, no extra fields.
cd "$OUT"
find $ID.artifactbundle -exec touch -h -t 202611190000 {} +
rm -f "$W/dist/$ID.artifactbundle.zip"
find $ID.artifactbundle | LC_ALL=C sort | zip -X -q -@ "$W/dist/$ID.artifactbundle.zip"
shasum -a 256 "$W/dist/$ID.artifactbundle.zip"
