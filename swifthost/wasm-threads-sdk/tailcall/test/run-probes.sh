#!/bin/bash
# Stack-depth probes for the tail-call SDK. Compiles pa/pb/pe/pf with a swiftc against a threads bundle, validates the
# modules with node (if present) and runs them in an iwasm built with -DWAMR_BUILD_TAIL_CALL=1.
# Usage: W=<build dir> run-probes.sh <label> <swiftc> <bundle-dir> <bundle-id> <tc|notc> <iwasm>
# tc adds -Xcc -mtail-call (what the toolset passes). Link: shared memory defined and exported.
set -u
W=${W:?set W to the build directory}
HERE=$(cd "$(dirname "$0")" && pwd)
L=$1; SWC=$2; BUNDLE=$3; ID=$4; MODE=$5; IW=$6
B=$BUNDLE/$ID/wasm32-unknown-wasip1-threads
OUT=$W/evidence/tailcall-probes-$L; rm -rf $OUT; mkdir -p $OUT
CC=(-Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec)
[ "$MODE" = tc ] && CC+=(-Xcc -mtail-call)
for p in pa_await_loop pb_actor_hops pe_patterns pf_coop_timer; do
  S=($HERE/$p.swift); [ $p != pf_coop_timer ] && S=($HERE/common.swift "${S[@]}")
  "$SWC" -target wasm32-unknown-wasip1-threads -sdk $B/WASI.sdk -resource-dir $B/swift.xctoolchain/usr/lib/swift_static \
    -static-stdlib -Xclang-linker -resource-dir -Xclang-linker $B/swift.xctoolchain/usr/lib/swift_static/clang "${CC[@]}" \
    -O -parse-as-library -module-cache-path $OUT/modcache "${S[@]}" -o $OUT/$p.wasm \
    -Xlinker --shared-memory -Xlinker --export-memory -Xlinker --max-memory=268435456 > $OUT/$p.compile.log 2>&1
  v=; command -v node >/dev/null && v=$(node $HERE/validate.mjs $OUT/$p.wasm)
  echo "$L $p compile=$? ld-signature-mismatch=$(grep -c 'signature mismatch' $OUT/$p.compile.log) node=${v:-skipped}"
done
run() { # wasm stack args...
  local f=$1 s=$2; shift 2
  timeout 300 $IW --stack-size=$s $f "$@" > $OUT/run.txt 2>&1; local rc=$?
  echo "$L $(basename $f .wasm) stack=$s args=[$*] rc=$rc: $(grep -E '^p[a-f]|xception|rror' $OUT/run.txt | tail -2 | tr '\n' '|')"
}
run $OUT/pa_await_loop.wasm 1048576            # 1,000,000 awaits
run $OUT/pb_actor_hops.wasm 1048576            # 100,000 actor hops
run $OUT/pa_await_loop.wasm 65536 10000000     # flat stack: 10M awaits in 64 KiB
run $OUT/pb_actor_hops.wasm 65536 1000000
for t in witness closure throwing generic cont ucont asynclet group mainactor; do run $OUT/pe_patterns.wasm 1048576 $t; done
run $OUT/pf_coop_timer.wasm 1048576
