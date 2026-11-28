#!/bin/bash
# SDK-level check that the threads runtime tells the main pthread from other pthreads (see "Changes in -8").
# Usage: isolation.sh <swiftc> <threads-sdk-dir> <out-dir> <iwasm>
#    (wasm-ld must be on PATH, as for the other tests)
#  - compiles wasi_threads_main_thread_isolation.swift (the swift repository's regression test) against the SDK;
#  - runs it: MainActor.assumeIsolated must pass on the main thread (prints "main") and trap on a pthread with
#    "Unexpected isolation context", so "OK" is never printed. On -7 the pthread prints "OK" and the run exits 0;
#  - Swift 6 language mode: swift6_closure_entry.swift (a top-level closure as pthread_create entry, @MainActor by
#    inference) must trap the same way, and swift6_global_entry.swift (a global function) must print "worker" and exit 0.
# Prints "isolation OK" when every expectation holds.
set -u
SWC=$1; B=$2; O=$3; IW=$4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$O"
SW=$B/swift.xctoolchain/usr/lib/swift_static
CC=(-Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call)
build() { # source out [extra swiftc args]
  local src=$1 out=$2; shift 2
  "$SWC" -target wasm32-unknown-wasip1-threads -sdk "$B/WASI.sdk" -resource-dir "$SW" -static-stdlib \
    -Xclang-linker -resource-dir -Xclang-linker "$SW/clang" "${CC[@]}" -module-cache-path "$O/modcache" "$@" \
    "$HERE/$src" -o "$O/$out.wasm" \
    -Xlinker --shared-memory -Xlinker --export-memory -Xlinker --max-memory=268435456 > "$O/$out.compile.log" 2>&1
}
run() { # out
  "$IW" --max-threads=8 "$O/$1.wasm" > "$O/$1.run.log" 2>&1
}
ok=1
traps() { # out: compiled, prints no "worker"/"OK" from the pthread, reports the isolation trap
  grep -Fq 'Unexpected isolation context' "$O/$1.run.log" || ok=0
  if grep -Fxq OK "$O/$1.run.log" || grep -Fxq worker "$O/$1.run.log"; then ok=0; fi
}

build wasi_threads_main_thread_isolation.swift isolation -parse-as-library; crc=$?
rrc=-; [ $crc = 0 ] && { run isolation; rrc=$?; }
echo "wasi_threads_main_thread_isolation.swift compile=$crc run=$rrc"
if [ $crc = 0 ]; then grep -Fxq main "$O/isolation.run.log" || ok=0; traps isolation; else ok=0; fi

build swift6_closure_entry.swift closure6 -swift-version 6; crc=$?
rrc=-; [ $crc = 0 ] && { run closure6; rrc=$?; }
echo "swift6_closure_entry.swift compile=$crc run=$rrc"
if [ $crc = 0 ]; then traps closure6; else ok=0; fi

build swift6_global_entry.swift global6 -swift-version 6; crc=$?
rrc=-; [ $crc = 0 ] && { run global6; rrc=$?; }
echo "swift6_global_entry.swift compile=$crc run=$rrc"
if [ $crc = 0 ] && [ $rrc = 0 ]; then grep -Fxq worker "$O/global6.run.log" || ok=0; else ok=0; fi

[ $ok = 1 ] && echo "isolation OK" || { echo "isolation FAILED"; tail -n 3 "$O"/*.run.log 2>/dev/null; }
