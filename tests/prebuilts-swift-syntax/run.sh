#!/bin/bash
# Runs inside the built FULL image (not the stock one). Usage: run.sh
# Gate for the SwiftPM swift-syntax prebuilts baked into the image (/opt/hyp3r/swiftpm-prebuilts, announced in
# $HYP3R_SWIFTPM_PREBUILTS_URL): a package with a macro target and an exact swift-syntax 604.0.0 dependency must build
# with `--experimental-prebuilts-download-url`, SwiftPM must take swift-syntax's MacroSupport from the prebuilt (so no
# SwiftSyntax/SwiftParser object directories exist in .build) and the macro must expand (the executable prints 42).
# A second build with --disable-experimental-prebuilts is informational (timing, and proof that the sources do get
# compiled without the prebuilt, which shows the assertion above can fail).
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
url="${HYP3R_SWIFTPM_PREBUILTS_URL:-}"
fail=0
bad() { echo "::error::$*"; fail=1; }
[ -n "$url" ] || { bad "HYP3R_SWIFTPM_PREBUILTS_URL is not set in the image"; echo "SUMMARY $(uname -m) prebuilt_used=no demo=no build_seconds=0 sources_build_seconds=0"; exit 1; }
swift --version 2>&1 | head -2
swiftc -print-target-info | grep -E '"(swiftCompilerTag|compilerVersion)"'
find "${url#file://}" -type f | sort

# build <label> <flags...> -> /tmp/<label>/ with a fresh copy and a fresh SwiftPM prebuilts cache
build() {
  local label=$1; shift
  local dir=/tmp/$label
  rm -rf "$dir" "$HOME/.swiftpm/prebuilts" "$HOME/.swiftpm/cache"; cp -r "$here/pkg" "$dir"
  local start; start=$(date +%s)
  ( cd "$dir" && swift build -c release -v "$@" ) > "$dir.log" 2>&1
  rc=$?
  secs=$(( $(date +%s) - start ))
  got=$("$dir/.build/release/demo" 2>&1)
  [ "$rc" = 0 ] || { bad "$label: swift build exit $rc"; tail -40 "$dir.log"; }
  [ "$got" = 42 ] || bad "$label: demo printed '$got', not 42"
}

# swift-syntax sources compiled = per-module object directories in the package build tree
compiled() { find "/tmp/$1/.build" -type d \( -name SwiftSyntax.build -o -name SwiftParser.build -o -name SwiftSyntaxMacros.build \) 2>/dev/null | wc -l; }

build with-prebuilts --experimental-prebuilts-download-url "$url"
with_secs=$secs; with_compiled=$(compiled with-prebuilts)
grep -iE 'prebuilt' /tmp/with-prebuilts.log | head -10
used=no
[ "$with_compiled" = 0 ] && grep -qiE 'prebuilt' /tmp/with-prebuilts.log && used=yes
[ "$used" = yes ] || { bad "prebuilt not used (swift-syntax module directories compiled: $with_compiled)"; tail -30 /tmp/with-prebuilts.log; }

build without-prebuilts --disable-experimental-prebuilts
without_secs=$secs; without_compiled=$(compiled without-prebuilts)
[ "$without_compiled" != 0 ] || echo "::warning::the build without prebuilts compiled no swift-syntax module either; the 'used' check proves less"

echo "SUMMARY $(uname -m) prebuilt_used=$used demo=$([ "$fail" = 0 ] && echo 42 || echo FAIL) with_prebuilts_seconds=$with_secs without_prebuilts_seconds=$without_secs sources_compiled_without=$without_compiled"
exit $fail
