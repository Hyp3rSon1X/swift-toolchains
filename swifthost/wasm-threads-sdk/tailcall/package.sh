#!/bin/bash
# Turns the generator's threads bundle (built with build-system.patch) into the tail-call release zip.
# Usage: W=<build dir> [GEN=<generator bundle>] [OUT=<dir>] package.sh   (after ../build.sh and ../gen-sdk.sh)
# Changes to the generator output, nothing else:
#  1. renamed swift-6.4.2-RELEASE_wasm-threads -> swift-6.4.2-RELEASE_wasm-threads-tailcall (bundle directory, artifact
#     directory, info.json artifact key and variant path); the triple stays wasm32-unknown-wasip1-threads;
#  2. toolset.json: -Xcc -mtail-call for swiftCompiler, -mtail-call for cCompiler and cxxCompiler, and (from -7)
#     -fno-exceptions for cxxCompiler only (the SDK's libc++abi has no exception runtime);
#  3. the Embedded Swift variant removed (upstream 6.4.2 builds no Embedded stdlib for wasip1-threads).
set -euo pipefail
W=${W:?set W to the build directory}
OLD=swift-6.4.2-RELEASE_wasm-threads
ID=swift-6.4.2-RELEASE_wasm-threads-tailcall
GEN=${GEN:-$W/src/swift-sdk-generator/Bundles/$OLD.artifactbundle}   # where swift-sdk-generator writes it
OUT=${OUT:-$W/dist-tailcall}
STAGE=$OUT/final
ZIP=$OUT/$ID.artifactbundle.zip
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -R "$GEN" "$STAGE/$ID.artifactbundle"
B=$STAGE/$ID.artifactbundle
mv "$B/$OLD" "$B/$ID"
T=$B/$ID/wasm32-unknown-wasip1-threads
python3 -I -B - "$B/info.json" "$T/toolset.json" "$OLD" "$ID" <<'PY'
import json, sys, collections
info, toolset, old, new = sys.argv[1:]
OD = collections.OrderedDict
def load(p): return json.load(open(p), object_pairs_hook=OD)
def dump(p, d): open(p, 'w').write(json.dumps(d, indent=2, separators=(',', ' : ')))
d = load(info)
del d['artifacts'][old + '-embedded']
assert list(d['artifacts']) == [old], list(d['artifacts'])
art = d['artifacts'].pop(old)
for v in art['variants']:
    assert v['path'].startswith(old + '/'), v['path']
    v['path'] = new + v['path'][len(old):]
d['artifacts'][new] = art
dump(info, d)
t = load(toolset)
t['swiftCompiler']['extraCLIOptions'] += ['-Xcc', '-mtail-call']
t['cCompiler']['extraCLIOptions'] += ['-mtail-call']
t['cxxCompiler']['extraCLIOptions'] += ['-mtail-call']
t['cxxCompiler']['extraCLIOptions'] += ['-fno-exceptions']
dump(toolset, t)
PY
rm -rf "$T/swift.xctoolchain/usr/lib/swift/embedded" "$T/swift.xctoolchain/usr/lib/swift_static/embedded" \
  "$T/embedded-swift-sdk.json" "$T/embedded-toolset.json"
if grep -rIl --include='*.json' -- "$OLD[\"/]" "$B"; then echo "old id left in JSON" >&2; exit 1; fi
# Deterministic zip: fixed mtimes, sorted entries, no extra fields.
cd "$STAGE"
find $ID.artifactbundle -exec touch -h -t 202611190000 {} +
rm -f "$ZIP"
find $ID.artifactbundle | LC_ALL=C sort | zip -X -q -@ "$ZIP"
shasum -a 256 "$ZIP"
