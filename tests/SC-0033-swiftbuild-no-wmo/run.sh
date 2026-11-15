#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <patched swift-package>
# SC-0033: SwiftBuild decided whole-module optimization from build settings only. SwiftPM's release default is
# -Owholemodule, so a target that turns WMO off with .unsafeFlags(["-no-whole-module-optimization"]) got WMO outputs
# planned while the driver compiled non-WMO, and the build failed with "unable to open dependencies file
# (<Target>-primary.d)". pkg/ is that package (release-only unsafeFlags on its test target). Stock swift-package is
# informational (it should fail); the patched swift-package (SwiftBuild from Hyp3rSon1X/swift-build) must build it.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; patched_spm="$(realpath "$1")"
build() { # <label>
  rm -rf /tmp/pkg; cp -r "$here/pkg" /tmp/pkg
  ( cd /tmp/pkg && timeout 900 swift build -c release --build-tests --build-system swiftbuild ) > "/tmp/$1.log" 2>&1
}
build stock; rc=$?
if [ $rc != 0 ] && grep -q -- '-primary.d' /tmp/stock.log; then echo "stock: fails as expected (-primary.d)"
elif [ $rc = 0 ]; then echo "::warning::stock: builds (exit 0); the test proves less with this SwiftPM"
else echo "stock: exit $rc, other error"; tail -5 /tmp/stock.log; fi
cp "$patched_spm" /usr/bin/swift-package; chmod 0755 /usr/bin/swift-package
build patched; rc=$?
if [ $rc = 0 ]; then echo "patched: builds"; echo "SUMMARY SC-0033 PASS"
else echo "::error::patched: exit $rc"; tail -20 /tmp/patched.log; echo "SUMMARY SC-0033 FAIL"; exit 1; fi
