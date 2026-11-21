#!/bin/bash
# Runs test/smoke.c against one slice.
#
#   run-smoke.sh macosx          <lib> <sdk-target-dir> <iwasm> [work-dir]
#   run-smoke.sh iphonesimulator <lib> <sdk-target-dir> <iwasm> [work-dir]
#
# <lib> is a slice's libSwiftHostCompiler.a (or the prelinked SwiftHostCompiler.o).
# <sdk-target-dir> is .../swift-6.4.2-RELEASE_wasm.artifactbundle/swift-6.4.2-RELEASE_wasm/wasm32-unknown-wasip1.
# <iwasm> is a macOS WAMR iwasm. The simulator run creates a temporary simulator device (named swifthost-smoke-<pid>,
# deleted on exit; see sim-device.sh, which also has the shared device of a test session), runs the test in it, and
# then runs the produced modules in iwasm on the host. Stopping the script (HUP, INT, TERM) stops the test.
# After the main run, each crash mode of smoke.c runs in a process of its own (each ends with the library poisoned),
# then the macro modes: plugin-handler (the app's own plugin message handler) and exec-plugin (executable plugins,
# run with test/nospawn.c inserted, which aborts on any process creation and logs it).
set -euo pipefail
SLICE=$1 LIB=$(cd "$(dirname "$2")" && pwd)/$(basename "$2") SDK=$3 IWASM=$4
HERE=$(cd "$(dirname "$0")" && pwd)
INC=$(cd "$HERE/../include" && pwd)
. "$HERE/sim-device.sh"
WORK=${5:-$(mktemp -d)}
mkdir -p "$WORK"
case $SLICE in
  macosx)          TRIPLE=arm64-apple-macosx27.0;;
  iphonesimulator) TRIPLE=arm64-apple-ios27.0-simulator;;
  *) echo "slice must be macosx or iphonesimulator" >&2; exit 2;;
esac
SYSROOT=$(xcrun --sdk "$SLICE" --show-sdk-path)
TOOLCHAIN=$(dirname "$(dirname "$(xcrun -f swiftc)")")
xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -I"$INC" -O1 -c "$HERE/smoke.c" -o "$WORK/smoke.o"
xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -O1 -c "$HERE/plugin-server.c" -o "$WORK/plugin-server.o"
xcrun clang++ -target $TRIPLE -isysroot "$SYSROOT" "$WORK/smoke.o" "$WORK/plugin-server.o" "$LIB" \
  -L"$SYSROOT/usr/lib/swift" -L"$TOOLCHAIN/lib/swift/$SLICE" -dead_strip -o "$WORK/smoke"
# The in-process plugin server library, found by smoke next to itself.
xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -O1 -fvisibility=hidden -dynamiclib "$HERE/plugin-server.c" \
  -o "$WORK/libswifthost-test-plugin-server.dylib"
xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -O1 -dynamiclib "$HERE/nospawn.c" -o "$WORK/libnospawn.dylib"
echo "smoke binary: $(stat -f %z "$WORK/smoke") bytes"
MODES="overflow helper-crash xllvm-version plugin-handler"

# exec-plugin's stderr (the interposer's log) must not show any process creation.
check_nospawn() {
  if grep -E 'NOSPAWN: (posix_spawnp?|v?fork|execve|execvp|system|popen) called' "$1" >/dev/null; then
    echo "FAIL exec-plugin: a process was created:"; grep NOSPAWN "$1"; return 1
  fi
  echo "exec-plugin: no process created"
}

if [[ $SLICE == macosx ]]; then
  rc=0
  proc_run "$WORK/smoke" "$SDK" "$WORK/run" "$IWASM" "$HERE" || rc=1
  for mode in $MODES; do
    proc_run "$WORK/smoke" "$SDK" "$WORK/run-$mode" "$IWASM" "$HERE" "$mode" ||
      { echo "FAIL $mode: exit status $?"; rc=1; }
  done
  proc_run env DYLD_INSERT_LIBRARIES="$WORK/libnospawn.dylib" "$WORK/smoke" "$SDK" "$WORK/run-exec-plugin" - "$HERE" \
    exec-plugin 2> "$WORK/exec-plugin.stderr" || { echo "FAIL exec-plugin: exit status $?"; rc=1; }
  check_nospawn "$WORK/exec-plugin.stderr" || rc=1
  exit $rc
fi

# A temporary iPhone device of the newest iOS runtime (or the shared one), deleted again when this script exits
# (sim-device.sh).
sim_device_create swifthost-smoke || exit 2
UDID=$SIM_UDID
rc=0
proc_run xcrun simctl spawn "$UDID" "$WORK/smoke" "$SDK" "$WORK/run" - "$HERE" || rc=$?
for mode in $MODES; do
  proc_run xcrun simctl spawn "$UDID" "$WORK/smoke" "$SDK" "$WORK/run-$mode" - "$HERE" "$mode" ||
    { echo "FAIL $mode: exit status $?"; rc=1; }
done
proc_run env SIMCTL_CHILD_DYLD_INSERT_LIBRARIES="$WORK/libnospawn.dylib" xcrun simctl spawn "$UDID" "$WORK/smoke" \
  "$SDK" "$WORK/run-exec-plugin" - "$HERE" exec-plugin 2> "$WORK/exec-plugin.stderr" ||
  { echo "FAIL exec-plugin: exit status $?"; rc=1; }
check_nospawn "$WORK/exec-plugin.stderr" || rc=1

# The modules built in the simulator, run on the host.
hello=$'hi'
foundation='hi from Foundation 1970-01-01T00:00:00Z {"n":42} example.org'
for w in "$WORK"/run/hello-onone-*.wasm "$WORK"/run/hello-after-errors.wasm "$WORK"/run/foundation-*.wasm \
         "$WORK"/run/macro-dylib.wasm "$WORK"/run/macro-reply-brackets.wasm "$WORK"/run/macro-after-bad-replies.wasm "$WORK"/run-plugin-handler/macro-handler*.wasm; do
  [[ -f $w ]] || continue
  out=$("$IWASM" "$w")
  case $(basename "$w") in hello*) want=$hello;; macro*) want=42;; *) want=$foundation;; esac
  if [[ $out == "$want" ]]; then echo "host run ok: $(basename "$w")"; else echo "FAIL host run $(basename "$w"): $out"; rc=1; fi
done
exit $rc
