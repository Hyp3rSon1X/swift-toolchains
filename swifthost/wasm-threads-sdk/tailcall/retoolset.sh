#!/bin/bash
# Derives a tail-call release zip from a previous tail-call release zip by applying only the toolset step of package.sh
# (-fno-exceptions appended to cxxCompiler, if absent), then the same deterministic zip step. Nothing else changes.
# Usage: PREV=<previous release zip> OUT=<dir> [ADD=0] retoolset.sh   (ADD=0 skips the toolset step: the zip step alone)
set -euo pipefail
PREV=${PREV:?set PREV to the previous release zip}
OUT=${OUT:?set OUT to the output directory}
ADD=${ADD:-1}
ID=swift-6.4.2-RELEASE_wasm-threads-tailcall
STAGE=$OUT/final
ZIP=$OUT/$ID.artifactbundle.zip
rm -rf "$STAGE"; mkdir -p "$STAGE"
unzip -q "$PREV" -d "$STAGE"
B=$STAGE/$ID.artifactbundle
T=$B/$ID/wasm32-unknown-wasip1-threads
if [ "$ADD" = 1 ]; then
python3 -I -B - "$T/toolset.json" <<'PY'
import json, sys, collections
OD = collections.OrderedDict
toolset = sys.argv[1]
def load(p): return json.load(open(p), object_pairs_hook=OD)
def dump(p, d): open(p, 'w').write(json.dumps(d, indent=2, separators=(',', ' : ')))
t = load(toolset)
o = t['cxxCompiler']['extraCLIOptions']
if '-fno-exceptions' not in o: o.append('-fno-exceptions')
dump(toolset, t)
PY
fi
# Deterministic zip: fixed mtimes, sorted entries, no extra fields (same as package.sh).
cd "$STAGE"
find $ID.artifactbundle -exec touch -h -t 202611190000 {} +
rm -f "$ZIP"
find $ID.artifactbundle | LC_ALL=C sort | zip -X -q -@ "$ZIP"
shasum -a 256 "$ZIP"
