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

# The device (named swifthost-skd-test-<pid>) comes from ../sim-device.sh, which also deletes it on HUP, INT and TERM
# and deletes devices that killed runs left behind.
. "$(dirname "${BASH_SOURCE[0]}")/../sim-device.sh"
slice_device() {
  [ "$SLICE" = iphonesimulator ] && [ -z "$SIM_UDID" ] || return 0
  sim_device_create swifthost-skd-test || exit 2
}
slice_run() {
  if [ "$SLICE" = macosx ]; then "$@"; else slice_device; xcrun simctl spawn "$SIM_UDID" "$@"; fi
}
