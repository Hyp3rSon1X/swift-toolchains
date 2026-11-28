#!/bin/bash
# SDK-level check that the threads runtime tells the main pthread from other pthreads (see "Changes in -8").
# Usage: isolation.sh <swiftc> <threads-sdk-dir> <out-dir> <iwasm>
#    (wasm-ld must be on PATH, as for the other tests)
#  - compiles wasi_threads_main_thread_isolation.swift (the swift repository's regression test) against the SDK;
#  - runs it: MainActor.assumeIsolated must pass on the main thread (prints "main") and trap on a pthread with
#    "Unexpected isolation context", so "OK" is never printed. On -7 the pthread prints "OK" and the run exits 0.
# Prints "isolation OK" when every expectation holds.
set -u
SWC=$1; B=$2; O=$3; IW=$4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$O"
SW=$B/swift.xctoolchain/usr/lib/swift_static
CC=(-Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call)
"$SWC" -target wasm32-unknown-wasip1-threads -sdk "$B/WASI.sdk" -resource-dir "$SW" -static-stdlib \
  -Xclang-linker -resource-dir -Xclang-linker "$SW/clang" "${CC[@]}" -parse-as-library -module-cache-path "$O/modcache" \
  "$HERE/wasi_threads_main_thread_isolation.swift" -o "$O/isolation.wasm" \
  -Xlinker --shared-memory -Xlinker --export-memory -Xlinker --max-memory=268435456 > "$O/isolation.compile.log" 2>&1
crc=$?
rrc=-
if [ $crc = 0 ]; then
  "$IW" --max-threads=8 "$O/isolation.wasm" > "$O/isolation.run.log" 2>&1; rrc=$?
fi
echo "wasi_threads_main_thread_isolation.swift compile=$crc run=$rrc"
ok=1
[ $crc = 0 ] || ok=0
if [ $crc = 0 ]; then
  grep -Fxq main "$O/isolation.run.log" || ok=0
  grep -Fq 'Unexpected isolation context' "$O/isolation.run.log" || ok=0
  grep -Fxq OK "$O/isolation.run.log" && ok=0
fi
[ $ok = 1 ] && echo "isolation OK" || { echo "isolation FAILED"; cat "$O/isolation.run.log" 2>/dev/null; }
