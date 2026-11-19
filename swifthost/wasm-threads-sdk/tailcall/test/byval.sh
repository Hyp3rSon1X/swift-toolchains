#!/bin/bash
# Compile the byval probe with a clang and -mtail-call, report return_call use per function, run it.
# Usage: byval.sh <clang> <threads-sdk-dir> <out-dir> <iwasm> [llvm-objdump]
set -u
CLANG=$1; B=$2; O=$3; IW=$4; OD=${5:-$(dirname $CLANG)/llvm-objdump}; SRC=$(cd $(dirname $0) && pwd)/byval.c
mkdir -p $O
"$CLANG" --target=wasm32-wasip1-threads --sysroot=$B/WASI.sdk -resource-dir $B/swift.xctoolchain/usr/lib/swift_static/clang \
  -matomics -mbulk-memory -pthread -mtail-call -O2 $SRC -o $O/byval.wasm \
  -Wl,--shared-memory -Wl,--export-memory -Wl,--max-memory=268435456 > $O/byval.compile.log 2>&1 || { cat $O/byval.compile.log; exit 1; }
"$OD" -d $O/byval.wasm > $O/byval.dis
for f in fwd_param fwd_local deep tail_ctl; do
  echo "$f: $(awk -v f="<$f>:" '$2==f{p=1;next} p&&/^[0-9a-f]+ </{exit} p' $O/byval.dis | grep -oE '\b(return_call|call)\b' | sort | uniq -c | tr '\n' ' ')"
done
"$IW" --stack-size=1048576 $O/byval.wasm; echo "rc=$?"
