#!/bin/bash
# Compile+link probes with the swift.org 6.4.2 swiftc against a threads bundle, then run in iwasm-threads.
# Usage: run-swiftorg.sh <label> <bundle-dir>
set -u
W=${W:?set W to the build directory}
HERE=$(cd "$(dirname "$0")" && pwd)
L=$1; BUNDLE=$2
B=$BUNDLE/swift-6.4.2-RELEASE_wasm-threads/wasm32-unknown-wasip1-threads
SWC=$W/tools/swiftorg-6.4.2/swift-6.4.2-RELEASE-osx-package.pkg/Payload/usr/bin/swiftc
IWASM=$W/tools/wamr-build-threads/iwasm
OUT=$W/evidence/swiftorg-$L; rm -rf $OUT; mkdir -p $OUT
CC=(-Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec)
LV1=(--import-memory --export-memory --shared-memory --max-memory=1073741824)
LV2=(--shared-memory --export-memory --max-memory=268435456)
for p in p1_taskgroup p2_atomic p3_mutex p4_foundation; do
 for v in v1 v2; do
  if [ $v = v1 ]; then LF=("${LV1[@]}"); else LF=("${LV2[@]}"); fi
  X=(); for f in "${LF[@]}"; do X+=(-Xlinker $f); done
  MC=$OUT/modcache-$p-$v; rm -rf $MC
  $SWC -target wasm32-unknown-wasip1-threads -sdk $B/WASI.sdk -resource-dir $B/swift.xctoolchain/usr/lib/swift_static \
    -static-stdlib -Xclang-linker -resource-dir -Xclang-linker $B/swift.xctoolchain/usr/lib/swift_static/clang "${CC[@]}" -parse-as-library -Rmodule-interface-rebuild -module-cache-path $MC \
    $HERE/$p.swift -o $OUT/$p-$v.wasm "${X[@]}" > $OUT/$p-$v.compile.log 2>&1
  echo "$L $p $v compile rc=$? rebuild-remarks=$(grep -c 'rebuilding module\|building module .* from' $OUT/$p-$v.compile.log) modcache-swiftmodules=$(find $MC -name '*.swiftmodule' 2>/dev/null | wc -l)"
 done
done
