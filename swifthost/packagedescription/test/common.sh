# Shared helpers for the manifest smoke tests. Source this file; do not run it.

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MANIFESTS=$HERE/manifests

# swift-tools-version of a manifest, e.g. 6.1
tools_version() { sed -n 's#^// swift-tools-version: *\([0-9.]*\).*#\1#p' "$1" | head -1; }

# The language mode SwiftPM compiles a manifest in: 5 below tools version 6.0, 6 from there on.
swift_language_version() { case "$1" in 3.*|4.*|5.*) echo 5 ;; *) echo 6 ;; esac; }

# The `-context` argument SwiftPM passes to a manifest, with fixed values.
CONTEXT_JSON='{"packageDirectory":"/work/pkg","gitInformation":{"currentTag":"1.2.3","currentCommit":"0123456789abcdef","hasUncommittedChanges":true}}'

# Each manifest is compiled as Package.swift in its own directory, as SwiftPM does.
stage_manifest() { # <manifest> <workdir>
  mkdir -p "$2"; cp "$1" "$2/Package.swift"
}

now_ms() { python3 -c 'import time; print(int(time.time()*1000))'; }

# Link flags of the SDK's toolset.json (linker.extraCLIOptions) and the run flags of the SDK tests (run-iwasm.sh).
LINK_FLAGS=(--import-memory --export-memory --shared-memory --max-memory=1073741824)
IWASM_FLAGS=(--stack-size=1048576 --heap-size=16777216)

ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time()*1000'; }

# run_wasm <manifest.wasm> <dir>: runs it in WAMR the way SwiftPM runs a manifest and fills RUN_RC, RUN_MS, RSS_MIB.
# The JSON goes to <dir>/manifest.json, the runtime's stderr and /usr/bin/time -l output to <dir>/run.err.
run_wasm() {
  local t0 t1
  t0=$(ms)
  /usr/bin/time -l "$IWASM" "${IWASM_FLAGS[@]}" "$1" -context "$CONTEXT_JSON" -fileno 1 > "$2/manifest.json" 2> "$2/run.err"
  RUN_RC=$?
  t1=$(ms)
  RUN_MS=$((t1 - t0))
  RSS_MIB=$(awk '/maximum resident set size/ { printf "%.1f", $1 / 1048576 }' "$2/run.err")
}

# compare_ref <name> <dir>: JSON equal to the host reference, byte for byte. Sets EQUAL to yes or no.
compare_ref() {
  if cmp -s "$REF/$1/manifest.json" "$2/manifest.json"; then EQUAL=yes; else EQUAL=no; diff <(tr ',' '\n' < "$REF/$1/manifest.json") <(tr ',' '\n' < "$2/manifest.json") > "$2/json.diff" 2>&1; fi
}
