#!/bin/bash
# Compiles, links and runs every manifest with swiftc against a built variant, and compares the JSON with the host
# reference (host-ref.sh).
# Usage: SDK_BUNDLE=<dir> SWIFTC=<path> TOOLS=<dir> IWASM=<path> REF=<host-ref out dir> smoke-swiftc.sh <variant dir> <out dir>
#   <variant dir> is $OUT/variants/<variant> from build.sh (or the unpacked release zip).
# Writes <out dir>/results.tsv: manifest, tools version, compile rc, wasm-ld warnings, signature mismatches,
# interface rebuilds, compile+link ms, wasm bytes, run rc, run ms, peak RSS MiB, JSON equal to the host reference.
set -u
. "$(dirname "$0")/common.sh"
VARIANT_DIR=$(cd "${1:?variant dir}" && pwd); OUT=${2:?out dir}
: "${SDK_BUNDLE:?}" "${SWIFTC:?}" "${TOOLS:?}" "${IWASM:?}" "${REF:?}"
export PATH=$TOOLS:$PATH
SDK_ID=swift-6.4.2-RELEASE_wasm-threads-tailcall; TRIPLE=wasm32-unknown-wasip1-threads
B=$SDK_BUNDLE/$SDK_ID/$TRIPLE; SR=$B/swift.xctoolchain/usr/lib/swift_static
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
COMMON=(-target $TRIPLE -sdk "$B/WASI.sdk" -resource-dir "$SR" -static-stdlib -Xclang-linker -resource-dir -Xclang-linker "$SR/clang"
  -Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call)
LINK=(); for f in "${LINK_FLAGS[@]}"; do LINK+=(-Xlinker "$f"); done
LIBS=(); while read -r l; do [ -n "$l" ] && LIBS+=("$l"); done < "$VARIANT_DIR/lib/link-libs.txt"
: > "$OUT/results.tsv"
for m in "$MANIFESTS"/*.swift; do
  n=$(basename "$m" .swift); tv=$(tools_version "$m"); sv=$(swift_language_version "$tv")
  D=$OUT/$n; rm -rf "$D"; mkdir -p "$D"; stage_manifest "$m" "$D"
  t0=$(ms)
  ( cd "$D" && "$SWIFTC" "${COMMON[@]}" -module-name main -swift-version "$sv" -package-description-version "$tv" \
      -I "$VARIANT_DIR" -L "$VARIANT_DIR/lib" -lCompilerPluginSupport -lPackageDescription "${LIBS[@]}" \
      -module-cache-path "$OUT/modulecache" -Xfrontend -Rmodule-interface-rebuild \
      Package.swift -o manifest.wasm "${LINK[@]}" > compile.log 2>&1 ); crc=$?
  t1=$(ms)
  warn=$(grep -c 'wasm-ld: warning' "$D/compile.log"); sig=$(grep -c 'signature mismatch' "$D/compile.log")
  rebuild=$(grep -c 'rebuilding module\|building module .* from\|compiling .* from its module interface' "$D/compile.log")
  RUN_RC=-; RUN_MS=-; RSS_MIB=-; EQUAL=no; size=-
  if [ $crc = 0 ]; then size=$(stat -f %z "$D/manifest.wasm"); run_wasm "$D/manifest.wasm" "$D"; compare_ref "$n" "$D"; fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$n" "$tv" "$crc" "$warn" "$sig" "$rebuild" $((t1 - t0)) "$size" "$RUN_RC" "$RUN_MS" "$RSS_MIB" "$EQUAL" | tee -a "$OUT/results.tsv"
done
