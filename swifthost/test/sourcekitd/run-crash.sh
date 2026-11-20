#!/bin/bash
# Crash containment and coexistence test of the in-process sourcekitd (skd-crash.c), macOS: builds
# skd-crash against a SwiftHostCompiler.o built with SOURCEKIT=1, prepares its inputs and runs every
# mode in a process of its own. Prints one row per mode; exit status 0 when every mode passed. The
# receiver-crash and notification-crash modes pass when their process ends with SIGTRAP from the app's
# code, outside the library's crash recovery.
#
#   run-crash.sh <SwiftHostCompiler.o or .a> <sdk-target-dir> <work-dir> [<long-compile-dir>]
#
# <sdk-target-dir> is the .../wasm32-unknown-wasip1-threads directory of the wasm-threads Swift SDK
# (as for run-smoke.sh). <long-compile-dir> holds the .swift files of a module whose compile takes a
# few seconds or more (the during-compile mode runs only with it). SRC is the source root of build.sh
# (default ./src): sourcekitd.h, and the completion crashers of swift's validation-test/IDE/crashers.
set -uo pipefail
LIB=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
SDK=$2 WORK=$(mkdir -p "$3" && cd "$3" && pwd) LONG=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "${SRC:-src}" && pwd)
INC=$(cd "$HERE/../../include" && pwd)
SKD_INC=$SRC/swift/tools/SourceKit/tools/sourcekitd/include
SYSROOT=$(xcrun --sdk macosx --show-sdk-path)
TOOLCHAIN=$(dirname "$(dirname "$(xcrun -f swiftc)")")
MC=$WORK/mc
mkdir -p "$MC" "$WORK/in"

xcrun clang -target arm64-apple-macosx27.0 -isysroot "$SYSROOT" -I"$SKD_INC" -I"$INC" -fblocks -O1 -Wall -Wextra \
  -c "$HERE/skd-crash.c" -o "$WORK/skd-crash.o" || exit 2
xcrun clang -target arm64-apple-macosx27.0 -isysroot "$SYSROOT" -O1 -c "$HERE/../plugin-server.c" \
  -o "$WORK/plugin-server.o" || exit 2
xcrun clang++ -target arm64-apple-macosx27.0 -isysroot "$SYSROOT" "$WORK/skd-crash.o" "$WORK/plugin-server.o" \
  "$LIB" -L"$SYSROOT/usr/lib/swift" -L"$TOOLCHAIN/lib/swift/macosx" -dead_strip -o "$WORK/skd-crash" || exit 2

# Completion crashers that still crash Swift 6.4.2 in a release build (SIGSEGV): their #^^# token is
# the completion point.
strip_token() { # <crasher> <out>: writes the source without its tokens, prints the first token's offset
  python3 -I -c '
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r"#\^[^^]*\^#", text)
body = text[:m.start()] + re.sub(r"#\^[^^]*\^#", "", text[m.end():])
open(sys.argv[2], "w").write(body)
print(len(text[:m.start()].encode()))' "$1" "$2"
}
CRASHERS=$SRC/swift/validation-test/IDE/crashers
SYNC_CRASHER=AbstractStorageDecl-setSynthesizedAccessor-89642f.swift
ASYNC_CRASHER=DeclContext-getASTContext-11c553.swift
for c in $SYNC_CRASHER $ASYNC_CRASHER; do
  [ -f "$CRASHERS/$c" ] || { echo "crasher $c not found in $CRASHERS (fixed upstream?)"; exit 2; }
done
SYNC_OFF=$(strip_token "$CRASHERS/$SYNC_CRASHER" "$WORK/in/$SYNC_CRASHER")
ASYNC_OFF=$(strip_token "$CRASHERS/$ASYNC_CRASHER" "$WORK/in/$ASYNC_CRASHER")

# A chain of 20000 typealiases, each the optional of the previous one: resolving the last one
# recurses through all of them. A chain of 2000 takes minutes to type-check (the request in flight).
chain() { # <n> <tail>
  python3 -I -c '
import sys
n = int(sys.argv[1])
print("typealias T0 = Int")
for i in range(1, n):
    print(f"typealias T{i} = T{i-1}?")
print(f"let z: T{n-1} = nil")
sys.stdout.write(sys.argv[2])' "$1" "$2"
}
chain 20000 "let w = z." > "$WORK/in/deep.swift"
chain 2000 "" > "$WORK/in/slow.swift"

# Frontend calls: a small module, and the long one.
frontend_args() { # <object> <files...>
  local obj=$1; shift
  printf '%s\n' swift-frontend -c "$@" -target wasm32-unknown-wasip1-threads -disable-objc-interop \
    -sdk "$SDK/WASI.sdk" -resource-dir "$SDK/swift.xctoolchain/usr/lib/swift_static" -use-static-resource-dir \
    -module-cache-path "$MC" -no-color-diagnostics -Xcc -fno-color-diagnostics -empty-abi-descriptor \
    -no-auto-bridging-header-chaining -module-name main -Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model \
    -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call -o "$obj"
}
frontend_args "$WORK/hello.o" "$HERE/../hello.swift" > "$WORK/in/hello.args"
long=()
if [ -n "$LONG" ]; then
  frontend_args "$WORK/long.o" "$LONG"/*.swift > "$WORK/in/long.args"
  long=(--long-compile-args "$WORK/in/long.args" --long-object "$WORK/long.o")
fi

common=(--args "$HERE/compilerargs.txt" --sdk "$SDK" --mc "$MC" --compile-args "$WORK/in/hello.args"
  --object "$WORK/hello.o")
bad=0
run() { # <mode> <options...>
  local mode=$1; shift
  local t0=$SECONDS
  "$WORK/skd-crash" "$mode" "${common[@]}" --work "$WORK/$mode" "$@" > "$WORK/$mode.log" 2>&1
  local rc=$? pass fail
  pass=$(grep -c '^PASS ' "$WORK/$mode.log") fail=$(grep -c '^FAIL ' "$WORK/$mode.log")
  printf '%-18s exit=%-3s checks: %2d pass, %d fail (%ds)\n' "$mode" "$rc" "$pass" "$fail" $((SECONDS - t0))
  [ "$rc" = 0 ] && [ "$fail" = 0 ] && [ "$pass" -gt 0 ] || { bad=$((bad + 1)); grep '^FAIL ' "$WORK/$mode.log"; }
}
run_dies() { # <mode>
  local mode=$1; shift
  local t0=$SECONDS
  "$WORK/skd-crash" "$mode" "${common[@]}" --work "$WORK/$mode" "$@" > "$WORK/$mode.log" 2>&1
  local rc=$? outside
  outside=$(grep -c '^TRAP .* outside ' "$WORK/$mode.log")
  printf '%-18s exit=%-3s trap outside crash recovery: %s (%ds)\n' "$mode" "$rc" "$outside" $((SECONDS - t0))
  # 128 + SIGTRAP: the trap was not recovered.
  [ "$rc" = 133 ] && [ "$outside" = 1 ] && ! grep -q '^FAIL ' "$WORK/$mode.log" ||
    { bad=$((bad + 1)); grep -E '^(FAIL|TRAP) ' "$WORK/$mode.log"; }
}
run crash-sync --crasher "$WORK/in/$SYNC_CRASHER" --crasher-offset "$SYNC_OFF"
run crash-async --crasher "$WORK/in/$ASYNC_CRASHER" --crasher-offset "$ASYNC_OFF"
run overflow --overflow "$WORK/in/deep.swift" --slow "$WORK/in/slow.swift"
run fatal
run frontend-fatal
run macro "${long[@]}"
run app-handler
run attribution --crasher "$WORK/in/$SYNC_CRASHER" --crasher-offset "$SYNC_OFF"
run receivers
run_dies receiver-crash
run_dies notification-crash
if [ -n "$LONG" ]; then
  run during-compile --crasher "$WORK/in/$SYNC_CRASHER" --crasher-offset "$SYNC_OFF" "${long[@]}"
fi
echo "modes failing: $bad"
[ "$bad" = 0 ]
