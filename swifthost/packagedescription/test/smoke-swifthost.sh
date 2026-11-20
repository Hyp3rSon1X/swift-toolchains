#!/bin/bash
# Like smoke-swiftc.sh, but compiles and links with the SwiftHostCompiler macOS slice (three calls: frontend,
# autolink-extract, wasm-ld, as in its JOBS.txt) instead of swiftc.
# Usage: SDK_BUNDLE=<dir> IWASM=<path> DRV=<swifthost_drv> REF=<host-ref out dir> smoke-swifthost.sh <variant dir> <out dir>
# Writes <out dir>/results.tsv with the same columns as smoke-swiftc.sh (compile ms covers all three calls).
set -u
. "$(dirname "$0")/common.sh"
VARIANT_DIR=$(cd "${1:?variant dir}" && pwd); OUT=${2:?out dir}
: "${SDK_BUNDLE:?}" "${IWASM:?}" "${DRV:?}" "${REF:?}"
SDK_ID=swift-6.4.2-RELEASE_wasm-threads-tailcall; TRIPLE=wasm32-unknown-wasip1-threads
SDK=$SDK_BUNDLE/$SDK_ID/$TRIPLE; SW=$SDK/swift.xctoolchain/usr/lib/swift_static
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
LIBS=(); while read -r l; do [ -n "$l" ] && LIBS+=("$l"); done < "$VARIANT_DIR/lib/link-libs.txt"
: > "$OUT/results.tsv"
for m in "$MANIFESTS"/*.swift; do
  n=$(basename "$m" .swift); tv=$(tools_version "$m"); sv=$(swift_language_version "$tv")
  D=$OUT/$n; rm -rf "$D"; mkdir -p "$D"; stage_manifest "$m" "$D"
  t0=$(ms)
  printf '%s\n' swift-frontend -c "$D/Package.swift" -target $TRIPLE -disable-objc-interop -sdk "$SDK/WASI.sdk" \
    -resource-dir "$SW" -use-static-resource-dir -module-cache-path "$OUT/modulecache" -no-color-diagnostics -Xcc -fno-color-diagnostics \
    -empty-abi-descriptor -no-auto-bridging-header-chaining -module-name main -swift-version "$sv" -package-description-version "$tv" \
    -I "$VARIANT_DIR" -Rmodule-interface-rebuild \
    -Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call \
    -o "$D/main.o" > "$D/frontend.args"
  "$DRV" frontend "$D/frontend.args" "$D/frontend.out" "$D/frontend.err" 2> "$D/frontend.rc"; rc1=$?
  printf '%s\n' swift-autolink-extract "$D/main.o" -o "$D/main.autolink" > "$D/autolink.args"
  rc2=1; rc3=1
  if [ $rc1 = 0 ]; then
    "$DRV" autolink "$D/autolink.args" "$D/autolink.out" "$D/autolink.err" 2> "$D/autolink.rc"; rc2=$?
    { printf '%s\n' wasm-ld -m wasm32 -L"$SW/wasi" -L"$SDK/WASI.sdk/lib/wasm32-wasip1-threads" -L"$VARIANT_DIR/lib" \
        "$SDK/WASI.sdk/lib/wasm32-wasip1-threads/crt1-command.o" "$SW/wasi/wasm32/swiftrt.o" "$D/main.o"
      grep -v '^$' "$D/main.autolink"
      printf '%s\n' -lCompilerPluginSupport -lPackageDescription "${LIBS[@]}"
      printf '%s\n' -lswiftSwiftOnoneSupport -ldl -lc++ -lc++abi -lm -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks \
        --error-limit=0 --threads=1 --global-base=4096 --table-base=4096 -z stack-size=131072 "${LINK_FLAGS[@]}" -lpthread -lc \
        "$SW/clang/lib/wasip1/libclang_rt.builtins-wasm32.a" -o "$D/manifest.wasm"; } > "$D/ld.args"
    "$DRV" ld "$D/ld.args" "$D/ld.out" "$D/ld.err" 2> "$D/ld.rc"; rc3=$?
  fi
  t1=$(ms)
  crc=0; [ $rc1 = 0 ] && [ $rc2 = 0 ] && [ $rc3 = 0 ] || crc=$rc1$rc2$rc3
  cat "$D"/frontend.err "$D"/ld.err > "$D/compile.log" 2>/dev/null
  warn=$(grep -c 'wasm-ld: warning' "$D/compile.log"); sig=$(grep -c 'signature mismatch' "$D/compile.log")
  rebuild=$(grep -c 'rebuilding module\|building module .* from\|compiling .* from its module interface' "$D/compile.log")
  RUN_RC=-; RUN_MS=-; RSS_MIB=-; EQUAL=no; size=-
  if [ "$crc" = 0 ]; then size=$(stat -f %z "$D/manifest.wasm"); run_wasm "$D/manifest.wasm" "$D"; compare_ref "$n" "$D"; fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$n" "$tv" "$crc" "$warn" "$sig" "$rebuild" $((t1 - t0)) "$size" "$RUN_RC" "$RUN_MS" "$RSS_MIB" "$EQUAL" | tee -a "$OUT/results.tsv"
done
