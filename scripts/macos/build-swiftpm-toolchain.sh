#!/bin/bash
# SC-0033: build a "SwiftPM-only" macOS xctoolchain (our SwiftPM + SwiftBuild engine, NO compiler/stdlib).
# Select it with TOOLCHAINS=<bundle id>; compiler tools then fall back to Xcode's XcodeDefault toolchain.
#
# usage: build-swiftpm-toolchain.sh [SWIFT_BUILD_REF] [SWIFTPM_REF] [NAME]
#   SWIFT_BUILD_REF  ref in repos/swift-build            (default swift-6.4.1-RELEASE; e.g. a patched fork branch)
#   SWIFTPM_REF      ref in repos/swift-package-manager  (default hyp3r/release/6.4.1)
#   NAME             output name, no extension           (default swiftpm-sc0033-<yyyymmdd>)
# Output: build/sc0033/out/<NAME>.xctoolchain and <NAME>.tar.xz (+ .sha256)
# Env: BUNDLE_ID (default dev.hyp3r.swiftpm.sc0033test), TC_VERSION (default 6.4.1.<yyyymmdd>), SKIP_BUILD=1 (reassemble only)
# Compiler used for the build: the default `swift` on PATH (Xcode). Heavy steps run through build-slot.
set -euo pipefail

ROOT=/Volumes/MEA-SSD/Hyp3rSuite/SwiftCollection
W=$ROOT/build/sc0033
SBREF=${1:-swift-6.4.1-RELEASE}
PMREF=${2:-hyp3r/release/6.4.1}
STAMP=$(date +%Y%m%d)
NAME=${3:-swiftpm-sc0033-$STAMP}
BUNDLE_ID=${BUNDLE_ID:-dev.hyp3r.swiftpm.sc0033test}
TC_VERSION=${TC_VERSION:-6.4.1.$STAMP}
export PATH="$HOME/.local/bin:$PATH"

# Worktrees live under build/sc0033 so the shared checkouts in repos/ are never modified.
ensure_wt() { # <repo> <dir> <ref>
  local repo=$ROOT/repos/$1 dir=$W/$2 ref=$3
  if [ -e "$dir/.git" ]; then
    git -C "$dir" checkout --detach "$ref" >/dev/null 2>&1 || { git -C "$repo" fetch -q --all 2>/dev/null || true; git -C "$dir" checkout --detach "$ref"; }
  else
    git -C "$repo" worktree add --detach "$dir" "$ref"
  fi
}
ensure_wt swift-build swift-build "$SBREF"
ensure_wt swift-package-manager swiftpm "$PMREF"
echo "swift-build: $(git -C $W/swift-build rev-parse --short HEAD)  swiftpm: $(git -C $W/swiftpm rev-parse --short HEAD)"

cd "$W/swiftpm"
B="$W/swiftpm/.build/out/Products/Release"
if [ -z "${SKIP_BUILD:-}" ]; then
  # Make SwiftPM use the local swift-build checkout (its other deps resolve from the release/6.4.x branches).
  [ -e Packages/swift-build ] || swift package edit swift-build --path "$W/swift-build"
  S=$(date +%s)
  # swift-package-manager is the multi-call binary installed as swift-package; the two dylib products carry
  # PackageDescription/CompilerPluginSupport (ManifestAPI) and PackagePlugin (PluginAPI) built from this source.
  for p in swift-package-manager swiftpm-testing-helper PackageDescription PackagePlugin; do
    build-slot swift build -c release --product "$p"
  done
  echo "build seconds: $(( $(date +%s) - S ))"
fi
[ -x "$B/swift-package-manager" ] || { echo "missing $B/swift-package-manager"; exit 1; }

# ---- assemble ----
OUT=$W/out; TC=$OUT/$NAME.xctoolchain
rm -rf "$TC" "$OUT/$NAME.tar.xz" "$OUT/$NAME.tar.xz.sha256"
mkdir -p "$TC/usr/bin" "$TC/usr/share/pm" \
         "$TC/usr/lib/swift/pm/ManifestAPI" "$TC/usr/lib/swift/pm/PluginAPI"

cp "$B/swift-package-manager" "$TC/usr/bin/swift-package"
for t in swift-build swift-test swift-run swift-package-collection swift-package-registry swift-sdk swift-experimental-sdk; do
  ln -s swift-package "$TC/usr/bin/$t"
done
# swiftpm-testing-helper goes in usr/bin, not the upstream usr/libexec/swift/pm: UserToolchain.getTool also searches the
# toolchain's own bin dir, and a usr/libexec/swift directory makes `xcrun swift` resolve to that directory
# ("can't exec ... Permission denied") instead of falling back to Xcode's swift.
cp "$B/swiftpm-testing-helper" "$TC/usr/bin/"
# Resource bundles (xcspecs, platform plugins, crypto) are found next to ../share/pm (same as the swift.org layout).
cp -R "$B"/*.bundle "$TC/usr/share/pm/"
cp "$W/swiftpm/Utilities/config.json" "$TC/usr/share/pm/config.json"
cp "$B/libPackageDescription.dylib" "$TC/usr/lib/swift/pm/ManifestAPI/"
cp -R "$B/PackageDescription.swiftmodule" "$B/CompilerPluginSupport.swiftmodule" "$TC/usr/lib/swift/pm/ManifestAPI/"
cp "$B/libPackagePlugin.dylib" "$TC/usr/lib/swift/pm/PluginAPI/"
cp -R "$B/PackagePlugin.swiftmodule" "$TC/usr/lib/swift/pm/PluginAPI/"

# Drop machine-specific rpaths (the Metal cryptex path); keep /usr/lib/swift, @loader_path and the Xcode stdlib path.
for f in "$TC/usr/bin/swift-package" "$TC/usr/bin/swiftpm-testing-helper" "$TC"/usr/lib/swift/pm/*/lib*.dylib; do
  for rp in $(otool -l "$f" | awk '/LC_RPATH/{r=1} r&&/path /{print $2; r=0}' | grep -E "MetalToolchain|^/Users/|/Volumes/|/private/" || true); do
    install_name_tool -delete_rpath "$rp" "$f"
  done
  codesign --force -s - "$f" >/dev/null 2>&1
done

DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)
cat > "$TC/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Aliases</key>
	<array/>
	<key>CFBundleIdentifier</key>
	<string>$BUNDLE_ID</string>
	<key>CompatibilityVersion</key>
	<integer>2</integer>
	<key>CreatedDate</key>
	<date>$DATE</date>
	<key>DisplayName</key>
	<string>SwiftPM-only ($NAME; swift-build $(git -C $W/swift-build rev-parse --short HEAD), swiftpm $(git -C $W/swiftpm rev-parse --short HEAD); no compiler)</string>
	<key>ReportProblemURL</key>
	<string>https://bugs.swift.org/</string>
	<key>ShortDisplayName</key>
	<string>SwiftPM-only $NAME</string>
	<key>Version</key>
	<string>$TC_VERSION</string>
</dict>
</plist>
EOF
plutil -lint "$TC/Info.plist" >/dev/null

# Guards: no compiler, nothing pointing into the build tree.
for n in swiftc swift swift-frontend swift-driver; do [ ! -e "$TC/usr/bin/$n" ] || { echo "compiler tool $n present"; exit 1; }; done
if find "$TC" -type f \( -perm -u+x -o -name '*.dylib' \) -exec otool -L {} + 2>/dev/null | grep -E "^"$'\t'".*($W|/Users/mea)"; then echo "build-dir reference in load commands"; exit 1; fi

mkdir -p "$OUT"
( cd "$OUT" && tar -cJf "$NAME.tar.xz" "$NAME.xctoolchain" && shasum -a 256 "$NAME.tar.xz" | tee "$NAME.tar.xz.sha256" )
du -sh "$TC"
echo "toolchain: $TC"
