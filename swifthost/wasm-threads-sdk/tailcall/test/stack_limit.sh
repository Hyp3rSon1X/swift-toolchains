#!/bin/bash
# SDK-level check of the exported __stack_limit global (see "Changes in -4").
# Usage: stack_limit.sh <clang> <threads-sdk-dir> <out-dir> <iwasm> [swiftc]
#  - compiles stack_limit.c (wasi-libc's test/src/pthread_stack_limit.c) against the SDK with both link variants and
#    runs it: the main thread reads 0, each pthread reads the low end of its stack (default, pthread_attr_setstack and
#    guard 0), and the default guard is 8 KiB;
#  - checks with node that each module exports a global named __stack_limit;
#  - with a swiftc: builds ../../test/p1_taskgroup.swift, ../../test/p3_mutex.swift (four pthreads) and a Swift program
#    that starts no thread, runs them and reports whether each exports __stack_limit. A module has the export exactly
#    when it can start threads (imports wasi.thread-spawn, that is, links pthread_create).
# Prints "stack_limit OK" when every expectation holds.
set -u
CLANG=$1; B=$2; O=$3; IW=$4; SWC=${5:-}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $O
bad=0
export_kind() { # wasm -> "global", another kind, or "none"
  node -e 'const fs = require("fs");
const e = WebAssembly.Module.exports(new WebAssembly.Module(fs.readFileSync(process.argv[1]))).find(x => x.name === "__stack_limit");
console.log(e ? e.kind : "none");' "$1"
}
spawns() { # wasm -> 1 if it imports wasi.thread-spawn, else 0
  node -e 'const fs = require("fs");
const i = WebAssembly.Module.imports(new WebAssembly.Module(fs.readFileSync(process.argv[1])));
console.log(i.some(x => x.module === "wasi" && x.name === "thread-spawn") ? 1 : 0);' "$1"
}
LV1=(--import-memory --export-memory --shared-memory --max-memory=1073741824)
LV2=(--shared-memory --export-memory --max-memory=268435456)
for v in v1 v2; do
  if [ $v = v1 ]; then LF=("${LV1[@]}"); else LF=("${LV2[@]}"); fi
  X=(); for f in "${LF[@]}"; do X+=(-Wl,$f); done
  "$CLANG" --target=wasm32-wasip1-threads --sysroot=$B/WASI.sdk -resource-dir $B/swift.xctoolchain/usr/lib/swift_static/clang \
    -matomics -mbulk-memory -pthread -mtail-call -O2 $HERE/stack_limit.c -o $O/stack_limit-$v.wasm "${X[@]}" \
    > $O/stack_limit-$v.compile.log 2>&1; crc=$?
  k=none; rrc=-
  if [ $crc = 0 ]; then
    k=$(export_kind $O/stack_limit-$v.wasm)
    "$IW" --stack-size=1048576 $O/stack_limit-$v.wasm > $O/stack_limit-$v.run.log 2>&1; rrc=$?
  fi
  echo "stack_limit.c $v compile=$crc export=$k run=$rrc"
  [ $crc = 0 ] && [ $k = global ] && [ $rrc = 0 ] || bad=$((bad+1))
done
if [ -n "$SWC" ]; then
  SW=$B/swift.xctoolchain/usr/lib/swift_static
  CC=(-Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call)
  printf '@main struct NoThreads {\n    static func main() { print("no threads") }\n}\n' > $O/no_threads.swift
  for p in $HERE/../../test/p1_taskgroup.swift $HERE/../../test/p3_mutex.swift $O/no_threads.swift; do
    n=$(basename $p .swift)
    "$SWC" -target wasm32-unknown-wasip1-threads -sdk $B/WASI.sdk -resource-dir $SW -static-stdlib \
      -Xclang-linker -resource-dir -Xclang-linker $SW/clang "${CC[@]}" -parse-as-library -module-cache-path $O/modcache $p -o $O/$n.wasm \
      -Xlinker --shared-memory -Xlinker --export-memory -Xlinker --max-memory=268435456 > $O/$n.compile.log 2>&1; crc=$?
    k=none; sp=-; rrc=-
    if [ $crc = 0 ]; then
      k=$(export_kind $O/$n.wasm); sp=$(spawns $O/$n.wasm)
      "$IW" --stack-size=1048576 $O/$n.wasm > $O/$n.run.log 2>&1; rrc=$?
    fi
    echo "$n compile=$crc export=$k thread-spawn=$sp run=$rrc"
    want=none; [ "$sp" = 1 ] && want=global
    [ $crc = 0 ] && [ $rrc = 0 ] && [ $k = $want ] || bad=$((bad+1))
  done
fi
[ $bad = 0 ] && echo "stack_limit OK" || echo "stack_limit FAILED ($bad)"
