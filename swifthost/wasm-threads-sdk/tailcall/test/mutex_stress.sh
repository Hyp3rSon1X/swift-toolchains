#!/bin/bash
# Mutex hang stress for the threads SDK (swiftlang/swift fix "Take the wasm Mutex as contended after waiting").
# Compiles three programs against the bundle's real Synchronization.Mutex and runs each many times with a timeout; a run
# that does not finish in time counts as a hang. Expected with SDK -3: 0 hangs. With -2 the `std` loop hangs in most runs.
# Usage: W=<output dir> mutex_stress.sh <label> <swiftc> <bundle-dir> <bundle-id> <iwasm> [p3-runs ab-runs handoff-runs]
#   p3        ../../test/p3_mutex.swift as is (4 threads x 1,000 withLock), timeout 60 s
#   ab        mutex_ab.swift mode std, 4 threads x 100,000 withLock, timeout 20 s
#   handoff   WasmMutexHandoff.swift (the swift repository's regression test), 4 x 100,000, timeout 20 s
set -u
W=${W:?set W to the output directory}
HERE=$(cd "$(dirname "$0")" && pwd)
L=$1; SWC=$2; BUNDLE=$3; ID=$4; IW=$5; NP3=${6:-100}; NAB=${7:-50}; NHO=${8:-20}
B=$BUNDLE/$ID/wasm32-unknown-wasip1-threads
OUT=$W/evidence/mutex-$L; rm -rf $OUT; mkdir -p $OUT
CC=(-Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call -enable-experimental-feature Extern)   # Extern: mutex_ab.swift calls the wasm atomic wait/notify intrinsics
build() { # name source
  "$SWC" -target wasm32-unknown-wasip1-threads -sdk $B/WASI.sdk -resource-dir $B/swift.xctoolchain/usr/lib/swift_static \
    -static-stdlib -Xclang-linker -resource-dir -Xclang-linker $B/swift.xctoolchain/usr/lib/swift_static/clang "${CC[@]}" \
    -O -parse-as-library -module-cache-path $OUT/modcache $2 -o $OUT/$1.wasm \
    -Xlinker --shared-memory -Xlinker --export-memory -Xlinker --max-memory=268435456 > $OUT/$1.compile.log 2>&1 \
    || { echo "$L $1 compile failed"; cat $OUT/$1.compile.log; exit 1; }
}
loop() { # name runs timeout expected-output-fragment args...
  local n=$1 runs=$2 to=$3 want=$4; shift 4
  local ok=0 hang=0 bad=0 t0=$(date +%s)
  for ((i = 1; i <= runs; i++)); do
    out=$(timeout $to $IW --stack-size=1048576 --heap-size=16777216 $OUT/$n.wasm "$@" 2>&1); rc=$?
    if [ $rc -eq 124 ]; then hang=$((hang+1))
    elif [ $rc -eq 0 ] && echo "$out" | grep -q "$want"; then ok=$((ok+1))
    else bad=$((bad+1)); echo "$L $n run $i rc=$rc: $out" | head -3; fi
  done
  echo "$L $n runs=$runs ok=$ok hang=$hang bad=$bad ($(( $(date +%s) - t0 )) s)"
}
build p3_mutex $HERE/../../test/p3_mutex.swift
build mutex_ab $HERE/mutex_ab.swift
build handoff $HERE/WasmMutexHandoff.swift
loop p3_mutex $NP3 60 'p3 mutex total=4000'
loop mutex_ab $NAB 20 'total=400000 ' std 100000
loop handoff $NHO 20 'total=400000'
