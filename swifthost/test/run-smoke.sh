#!/bin/bash
# Runs test/smoke.c against one slice.
#
#   run-smoke.sh macosx          <lib> <sdk-target-dir> <iwasm> [work-dir]
#   run-smoke.sh iphonesimulator <lib> <sdk-target-dir> <iwasm> [work-dir]
#
# <lib> is a slice's libSwiftHostCompiler.a (or the prelinked SwiftHostCompiler.o).
# <sdk-target-dir> is .../swift-6.4.2-RELEASE_wasm.artifactbundle/swift-6.4.2-RELEASE_wasm/wasm32-unknown-wasip1.
# <iwasm> is a macOS WAMR iwasm. The simulator run creates a temporary simulator device, runs the test in it, deletes
# the device, and then runs the produced modules in iwasm on the host.
# After the main run, each crash mode of smoke.c runs in a process of its own (each ends with the library poisoned),
# then the macro modes: plugin-handler (the app's own plugin message handler) and exec-plugin (executable plugins,
# run with test/nospawn.c inserted, which aborts on any process creation and logs it).
set -euo pipefail
SLICE=$1 LIB=$(cd "$(dirname "$2")" && pwd)/$(basename "$2") SDK=$3 IWASM=$4
HERE=$(cd "$(dirname "$0")" && pwd)
INC=$(cd "$HERE/../include" && pwd)
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
  "$WORK/smoke" "$SDK" "$WORK/run" "$IWASM" "$HERE" || rc=1
  for mode in $MODES; do
    "$WORK/smoke" "$SDK" "$WORK/run-$mode" "$IWASM" "$HERE" "$mode" || { echo "FAIL $mode: exit status $?"; rc=1; }
  done
  DYLD_INSERT_LIBRARIES="$WORK/libnospawn.dylib" "$WORK/smoke" "$SDK" "$WORK/run-exec-plugin" - "$HERE" exec-plugin \
    2> "$WORK/exec-plugin.stderr" || { echo "FAIL exec-plugin: exit status $?"; rc=1; }
  check_nospawn "$WORK/exec-plugin.stderr" || rc=1
  exit $rc
fi

# An iOS runtime and an iPhone device type for it: among the available runtimes that support more than one
# iPhone (some runtimes support a single device, and simctl create rejects others), the newest version, and
# among those the one that supports the most device types.
read -r RUNTIME DEVTYPE < <(xcrun simctl list runtimes -j | python3 -I -c '
import json, sys
iphones = lambda r: [d["identifier"] for d in r.get("supportedDeviceTypes", []) if d["name"].startswith("iPhone")]
rs = [r for r in json.load(sys.stdin)["runtimes"] if r["platform"] == "iOS" and r["isAvailable"] and len(iphones(r)) > 1]
if not rs:
    sys.exit("no available iOS simulator runtime that supports iPhones")
r = max(rs, key=lambda r: ([int(x) for x in r["version"].split(".")], len(r["supportedDeviceTypes"])))
print(r["identifier"], iphones(r)[0])') || true
if [[ -z ${RUNTIME:-} || -z ${DEVTYPE:-} ]]; then
  echo "no usable iOS simulator runtime" >&2
  exit 2
fi
UDID=$(xcrun simctl create swifthost-smoke "$DEVTYPE" "$RUNTIME")
trap 'xcrun simctl shutdown "$UDID" >/dev/null 2>&1 || true; xcrun simctl delete "$UDID" || true' EXIT
xcrun simctl boot "$UDID"
xcrun simctl bootstatus "$UDID" >/dev/null
rc=0
xcrun simctl spawn "$UDID" "$WORK/smoke" "$SDK" "$WORK/run" - "$HERE" || rc=$?
for mode in $MODES; do
  xcrun simctl spawn "$UDID" "$WORK/smoke" "$SDK" "$WORK/run-$mode" - "$HERE" "$mode" || { echo "FAIL $mode: exit status $?"; rc=1; }
done
SIMCTL_CHILD_DYLD_INSERT_LIBRARIES="$WORK/libnospawn.dylib" xcrun simctl spawn "$UDID" "$WORK/smoke" "$SDK" \
  "$WORK/run-exec-plugin" - "$HERE" exec-plugin 2> "$WORK/exec-plugin.stderr" || { echo "FAIL exec-plugin: exit status $?"; rc=1; }
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
