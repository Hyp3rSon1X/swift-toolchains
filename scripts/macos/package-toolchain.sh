#!/bin/bash
# Usage: package-toolchain.sh <out-dir> <release-id> <version> <arch-label>
#   e.g. package-toolchain.sh build/macos-toolchain 6.4.0-hyp3r.4 6.4.20261010104 arm64
# Packs <out-dir>/swift-<release-id>.xctoolchain (from assemble-toolchain.sh) into
#   swift-<release-id>-macos-<arch>.xctoolchain.tar.xz   (extract into ~/Library/Developer/Toolchains)
#   swift-<release-id>-macos-<arch>.pkg                  (unsigned product archive; installs into
#                                                         /Library/Developer/Toolchains or, for "me only",
#                                                         ~/Library/Developer/Toolchains, like swift.org's .pkg)
# plus SHA256SUMS. The .pkg is not signed: there is no Developer ID Installer identity.
set -euo pipefail
[ $# -eq 4 ] || { sed -n '2,10p' "$0"; exit 2; }
OUT="$(cd "$1" && pwd)"; RELEASE_ID="$2"; VERSION="$3"; ARCH="$4"
NAME="swift-${RELEASE_ID}"; ID="dev.hyp3r.swift.${RELEASE_ID}"
TC="$OUT/$NAME.xctoolchain"
[ -d "$TC" ] || { echo "error: no $TC" >&2; exit 1; }
TAR="$OUT/$NAME-macos-$ARCH.xctoolchain.tar.xz"; PKG="$OUT/$NAME-macos-$ARCH.pkg"
W="$OUT/pkgwork"; R="$OUT/pkgroot"
rm -rf "$W" "$R" "$TAR" "$PKG"; mkdir -p "$W" "$R"
trap 'rm -rf "$W" "$R"' EXIT

# Tarball: no AppleDouble/xattr entries; code signatures live inside the Mach-Os and _CodeSignature.
(cd "$OUT" && COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs -cf - "$NAME.xctoolchain") | xz -T8 -6 > "$TAR"

# Component package. The bundle must not be relocatable, or Installer moves it onto any copy it finds.
cp -c -R "$TC" "$R/" 2>/dev/null || ditto "$TC" "$R/$NAME.xctoolchain"
pkgbuild --analyze --root "$R" "$W/component.plist"
plutil -replace 0.BundleIsRelocatable -bool NO "$W/component.plist"
pkgbuild --root "$R" --component-plist "$W/component.plist" --identifier "$ID" --version "$VERSION" \
  --install-location /Library/Developer/Toolchains "$W/$NAME.pkg"
cat > "$W/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>Swift ${RELEASE_ID%%-*} ${RELEASE_ID#*-}</title>
  <options customize="never" require-scripts="false" hostArchitectures="$ARCH"/>
  <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="true"/>
  <volume-check><allowed-os-versions><os-version min="13.0"/></allowed-os-versions></volume-check>
  <choices-outline><line choice="$ID"/></choices-outline>
  <choice id="$ID" visible="false"><pkg-ref id="$ID"/></choice>
  <pkg-ref id="$ID" version="$VERSION" onConclusion="none">$NAME.pkg</pkg-ref>
</installer-gui-script>
EOF
productbuild --distribution "$W/distribution.xml" --package-path "$W" "$PKG"

(cd "$OUT" && shasum -a 256 "$(basename "$TAR")" "$(basename "$PKG")" > SHA256SUMS && cat SHA256SUMS)
ls -l "$TAR" "$PKG"
