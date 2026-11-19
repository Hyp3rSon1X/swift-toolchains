#!/bin/bash
# Usage: run-iwasm.sh <outdir>  -- runs every *.wasm there in iwasm-threads, records output/exit.
IW=${W:?set W to the build directory}/tools/wamr-build-threads/iwasm
for f in $1/*.wasm; do
  b=$(basename $f .wasm)
  timeout 60 $IW --stack-size=1048576 --heap-size=16777216 $f > ${f%.wasm}.run.txt 2>&1; rc=$?
  echo "$b rc=$rc: $(grep -m2 -E '^p[0-9]|rror|xception|fail' ${f%.wasm}.run.txt | tr '\n' '|' | cut -c1-200)"
done
