# Sourced by the sourcekitd test scripts: selects the Apple slice they build for and run on.
#
#   SLICE=macosx (default)  run on the host
#   SLICE=iphonesimulator   build for arm64-apple-ios27.0-simulator and run every test process inside a temporary
#                           simulator device (xcrun simctl spawn; spawned processes read and write host paths)
#
# Provides SLICE, TRIPLE, SYSROOT, TOOLCHAIN_LIB (the toolchain's lib/swift/<slice>) and slice_run CMD..., which runs
# CMD on the slice. The device is created on first use of slice_run and deleted when the script exits.
SLICE=${SLICE:-macosx}
case $SLICE in
  macosx)          TRIPLE=arm64-apple-macosx27.0;;
  iphonesimulator) TRIPLE=arm64-apple-ios27.0-simulator;;
  *) echo "SLICE must be macosx or iphonesimulator" >&2; exit 2;;
esac
SYSROOT=$(xcrun --sdk "$SLICE" --show-sdk-path)
TOOLCHAIN_LIB=$(dirname "$(dirname "$(xcrun -f swiftc)")")/lib/swift/$SLICE
# SLICE_LINK_EXTRA: extra objects or flags for the link line of the test binaries (word-split), empty by default.
SLICE_LINK_EXTRA=${SLICE_LINK_EXTRA:-}

SLICE_UDID=
slice_device_cleanup() {
  [ -n "$SLICE_UDID" ] || return 0
  xcrun simctl shutdown "$SLICE_UDID" >/dev/null 2>&1 || true
  xcrun simctl delete "$SLICE_UDID" >/dev/null 2>&1 || true
}
# Same runtime choice as test/run-smoke.sh: among the available iOS runtimes that support more than one iPhone (some
# support a single device, and simctl create rejects others), the newest version, then the one with the most device types.
slice_device() {
  [ "$SLICE" = iphonesimulator ] && [ -z "$SLICE_UDID" ] || return 0
  local runtime devtype
  read -r runtime devtype < <(xcrun simctl list runtimes -j | python3 -I -c '
import json, sys
iphones = lambda r: [d["identifier"] for d in r.get("supportedDeviceTypes", []) if d["name"].startswith("iPhone")]
rs = [r for r in json.load(sys.stdin)["runtimes"] if r["platform"] == "iOS" and r["isAvailable"] and len(iphones(r)) > 1]
if not rs:
    sys.exit("no available iOS simulator runtime that supports iPhones")
r = max(rs, key=lambda r: ([int(x) for x in r["version"].split(".")], len(r["supportedDeviceTypes"])))
print(r["identifier"], iphones(r)[0])') || true
  if [ -z "${runtime:-}" ] || [ -z "${devtype:-}" ]; then echo "no usable iOS simulator runtime" >&2; exit 2; fi
  trap slice_device_cleanup EXIT
  SLICE_UDID=$(xcrun simctl create swifthost-skd-test "$devtype" "$runtime") || exit 2
  xcrun simctl boot "$SLICE_UDID" && xcrun simctl bootstatus "$SLICE_UDID" >/dev/null || exit 2
  echo "simulator device: $runtime $devtype" >&2
}
slice_run() {
  if [ "$SLICE" = macosx ]; then "$@"; else slice_device; xcrun simctl spawn "$SLICE_UDID" "$@"; fi
}
