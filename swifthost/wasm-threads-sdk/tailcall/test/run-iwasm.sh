#!/bin/bash
# Usage: run-iwasm.sh <outdir> <iwasm>
IW=${2:?iwasm built with -DWAMR_BUILD_TAIL_CALL=1 -DWAMR_BUILD_LIB_WASI_THREADS=1}
for f in $1/*.wasm; do
  b=$(basename $f .wasm)
  timeout 60 $IW --stack-size=1048576 --heap-size=16777216 $f > ${f%.wasm}.run.txt 2>&1; rc=$?
  echo "$b rc=$rc: $(grep -m2 -E '^p[0-9a-e]|rror|xception|fail' ${f%.wasm}.run.txt | tr '\n' '|' | cut -c1-200)"
done
