#!/bin/bash
# Runs the whole smoke test: host reference once, then both variants with swiftc and the essentials variant with
# SwiftHostCompiler, REPS times each, and prints the summary table and the ICU check.
# Usage: SDK_BUNDLE=<dir> SWIFTC=<path> TOOLS=<dir> IWASM=<path> DRV=<swifthost_drv> HOST_TOOLCHAIN=<usr dir> \
#          VARIANTS=<$OUT/variants> WORK=<dir> [REPS=3] run-all.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
: "${SDK_BUNDLE:?}" "${SWIFTC:?}" "${TOOLS:?}" "${IWASM:?}" "${DRV:?}" "${HOST_TOOLCHAIN:?}" "${VARIANTS:?}" "${WORK:?}"
REPS=${REPS:-3}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
export SDK_BUNDLE SWIFTC TOOLS IWASM DRV REF=$WORK/ref
"$HERE/host-ref.sh" "$HOST_TOOLCHAIN" "$REF" > "$WORK/host-ref.txt" || exit 1
grep -c 'compile=0 run=0' "$WORK/host-ref.txt" | sed 's/$/ host reference manifests ran/'
for rep in $(seq 1 "$REPS"); do
  "$HERE/smoke-swiftc.sh" "$VARIANTS/essentials" "$WORK/swiftc-essentials/rep$rep" > /dev/null
  "$HERE/smoke-swiftc.sh" "$VARIANTS/foundation" "$WORK/swiftc-foundation/rep$rep" > /dev/null
  "$HERE/smoke-swifthost.sh" "$VARIANTS/essentials" "$WORK/swifthost-essentials/rep$rep" > /dev/null
done
"$HERE/summarize.py" "swiftc essentials=$WORK/swiftc-essentials" "swiftc foundation=$WORK/swiftc-foundation" \
  "swifthost essentials=$WORK/swifthost-essentials"
echo
# Manifests that import only PackageDescription must carry no ICU in the essentials variant; 16 imports Foundation itself.
(cd "$WORK" && "$HERE/check-icu.sh" swiftc-essentials/rep1/*/manifest.wasm swifthost-essentials/rep1/*/manifest.wasm \
  swiftc-foundation/rep1/*/manifest.wasm > icu.tsv)
awk -F'\t' 'NR > 1 { split($1, p, "/"); k = p[1]; n[k]++; if ($2 + $3 + $4 + $5 > 0) icu[k]++ }
  END { for (k in n) printf "%s: %d manifests, %d with ICU, FoundationInternationalization or CoreFoundation code\n", k, n[k], icu[k] }' "$WORK/icu.tsv" | sort
