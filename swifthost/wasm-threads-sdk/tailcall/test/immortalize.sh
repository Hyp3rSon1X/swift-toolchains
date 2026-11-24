#!/bin/bash
# SDK-level check of _swift_stdlib_immortalize (see "Changes in -5").
# Usage: immortalize.sh <swiftc> <threads-sdk-dir> <out-dir> <iwasm>
#    (wasm-ld must be on PATH, as for the other tests)
#  - compiles immortalize.swift, which calls the function through `import SwiftShims` as swift-collections 1.7.6 does,
#    against the SDK and links it twice (plain, and with --fatal-warnings);
#  - both links must succeed without a "signature mismatch" warning (on -4 the --fatal-warnings link fails, and the plain
#    one traps with `unreachable` when the call runs);
#  - the program must report that the object stays immortal (unbalanced releases and retain/release pairs leave the
#    refcount word unchanged, deinit never runs).
# Prints "immortalize OK" when every expectation holds.
set -u
SWC=$1; B=$2; O=$3; IW=$4
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p $O
SW=$B/swift.xctoolchain/usr/lib/swift_static
CC=(-Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call)
bad=0
for v in plain fatal; do
  X=(); [ $v = fatal ] && X=(-Xlinker --fatal-warnings)
  "$SWC" -target wasm32-unknown-wasip1-threads -sdk $B/WASI.sdk -resource-dir $SW -static-stdlib \
    -Xclang-linker -resource-dir -Xclang-linker $SW/clang "${CC[@]}" -module-cache-path $O/modcache $HERE/immortalize.swift \
    -o $O/immortalize-$v.wasm -Xlinker --shared-memory -Xlinker --export-memory -Xlinker --max-memory=268435456 "${X[@]}" \
    > $O/immortalize-$v.compile.log 2>&1; crc=$?
  mm=$(grep -c 'signature mismatch' $O/immortalize-$v.compile.log)
  rrc=-
  if [ $crc = 0 ]; then
    "$IW" --stack-size=1048576 $O/immortalize-$v.wasm > $O/immortalize-$v.run.log 2>&1; rrc=$?
  fi
  echo "immortalize.swift $v compile=$crc mismatch-lines=$mm run=$rrc"
  [ $crc = 0 ] && [ $mm = 0 ] && [ $rrc = 0 ] || bad=$((bad+1))
done
[ $bad = 0 ] && echo "immortalize OK" || echo "immortalize FAILED ($bad)"
