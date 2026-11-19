#!/bin/bash
# Compile+link probes through SwiftHostCompiler (swifthost_drv) against a threads bundle (JOBS.txt recipe, threads target).
# Usage: run-swifthost.sh <label> <bundle-dir> <bundle-id> <drv> <tc|notc> [probe-dir] [probes...]
set -u
W=${W:?set W to the build directory}
HERE=$(cd "$(dirname "$0")" && pwd)
L=$1; ID=$3; SDK=$2/$ID/wasm32-unknown-wasip1-threads; MODE=$5; PD=${6:-$HERE/../../test}; PROBES=("${@:7}"); [ ${#PROBES[@]} = 0 ] && PROBES=(p1_taskgroup p2_atomic p3_mutex p4_foundation)
TC=(); [ "$MODE" = tc ] && TC=(-Xcc -mtail-call)
SW=$SDK/swift.xctoolchain/usr/lib/swift_static
DRV=$4
OUT=$W/evidence/tailcall-$L; rm -rf $OUT; mkdir -p $OUT; CACHE=$OUT/modcache
LV1=(--import-memory --export-memory --shared-memory --max-memory=1073741824)
LV2=(--shared-memory --export-memory --max-memory=268435456)
for p in "${PROBES[@]}"; do
 for v in v1 v2; do
  if [ $v = v1 ]; then LF=("${LV1[@]}"); else LF=("${LV2[@]}"); fi
  D=$OUT/$p-$v; mkdir -p $D
  printf '%s\n' swift-frontend -c $PD/$p.swift -target wasm32-unknown-wasip1-threads -disable-objc-interop -sdk $SDK/WASI.sdk \
    -resource-dir $SW -use-static-resource-dir -module-cache-path $CACHE -no-color-diagnostics -Xcc -fno-color-diagnostics \
    -empty-abi-descriptor -no-auto-bridging-header-chaining -module-name main -parse-as-library -O -enable-default-cmo \
    -Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec "${TC[@]}" \
    -o $D/main.o > $D/frontend.args
  $DRV frontend $D/frontend.args $D/frontend.out $D/frontend.err 2> $D/frontend.rc; rc1=$?
  printf '%s\n' swift-autolink-extract $D/main.o -o $D/main.autolink > $D/autolink.args
  $DRV autolink $D/autolink.args $D/autolink.out $D/autolink.err 2> $D/autolink.rc; rc2=$?
  { printf '%s\n' wasm-ld -m wasm32 -L$SW/wasi -L$SDK/WASI.sdk/lib/wasm32-wasip1-threads $SDK/WASI.sdk/lib/wasm32-wasip1-threads/crt1-command.o \
      $SW/wasi/wasm32/swiftrt.o $D/main.o
    grep -v '^$' $D/main.autolink
    printf '%s\n' -lswiftSwiftOnoneSupport -ldl -lc++ -lc++abi -lm -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks \
      --error-limit=0 --threads=1 --global-base=4096 --table-base=4096 -z stack-size=131072 "${LF[@]}" --shared-memory -lpthread -lc \
      $SW/clang/lib/wasip1/libclang_rt.builtins-wasm32.a -o $D/main.wasm; } > $D/ld.args
  $DRV ld $D/ld.args $D/ld.out $D/ld.err 2> $D/ld.rc; rc3=$?
  cp $D/main.wasm $OUT/$p-$v.wasm 2>/dev/null
  echo "$L $p $v frontend=$rc1 autolink=$rc2 ld=$rc3 ld-mismatch=$(grep -c "signature mismatch" $D/ld.err) interface-rebuilds=$(grep -c 'rebuilding\|swiftinterface' $D/frontend.err) modcache-swiftmodule=$(find $CACHE -name '*.swiftmodule' 2>/dev/null | wc -l | tr -d ' ')"
 done
done
