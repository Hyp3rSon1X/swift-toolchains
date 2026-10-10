#!/bin/bash
# Usage: assemble-toolchain.sh <base.xctoolchain> <swift build dir> <out-dir> <release-id> <version> <display-suffix>
#   e.g. assemble-toolchain.sh ~/Library/Developer/Toolchains/swift-6.4.0-RELEASE.xctoolchain \
#          <build-script build>/swift-macosx-arm64 build/macos-toolchain 6.4.0-hyp3r.4 6.4.20261010104 "SC-0004"
# Produces <out-dir>/swift-<release-id>.xctoolchain: a copy of the official swift.org toolchain in which only the
# compiler frontend is replaced: usr/bin/swift-frontend plus, in the new directory usr/lib/swift/host/compiler-hyp3r,
# the compiler's own swift-syntax libraries (lib_CompilerSwift*.dylib) from the same build. The frontend's rpath
# is pointed at that directory. Why: a frontend built by a different host Swift compiler than swift.org's does not
# link against the official lib_CompilerSwift*.dylib (it expects weak-def exports they lack), while LLDB,
# sourcekitd and the scanner library must keep loading the official copies in usr/lib/swift/host/compiler.
# Binaries are stripped like build-script's darwin-install-extract-symbols does and signed ad hoc. The bundle
# gets the identifier dev.hyp3r.swift.<release-id>, so it installs next to the official one and is selected with
# TOOLCHAINS=dev.hyp3r.swift.<release-id>; Aliases is emptied, the toolchain never claims "swift". The base
# toolchain is only read. The bundle seal is redone ad hoc; untouched binaries keep their Developer ID signatures.
set -euo pipefail
[ $# -eq 6 ] || { sed -n '2,16p' "$0"; exit 2; }
BASE="${1%/}"; BUILD="${2%/}"; FRONTEND="$BUILD/bin/swift-frontend"; mkdir -p "$3"; OUT="$(cd "$3" && pwd)"; RELEASE_ID="$4"; VERSION="$5"; SUFFIX="$6"
SWIFT_VERSION="${RELEASE_ID%%-*}"
ID="dev.hyp3r.swift.${RELEASE_ID}"
TC="$OUT/swift-${RELEASE_ID}.xctoolchain"

[ -f "$BASE/Info.plist" ] && [ -x "$BASE/usr/bin/swift-frontend" ] || { echo "error: $BASE is not a toolchain" >&2; exit 1; }
[ -x "$FRONTEND" ] || { echo "error: no executable $FRONTEND" >&2; exit 1; }
[ ! -e "$TC" ] || { echo "error: $TC exists; remove it first" >&2; exit 1; }

base_archs=$(lipo -archs "$BASE/usr/bin/swift-frontend"); new_archs=$(lipo -archs "$FRONTEND")
echo "base swift-frontend: $base_archs; new: $new_archs"
[ "$base_archs" = "$new_archs" ] || echo "warning: the toolchain will only run on: $new_archs"

ditto "$BASE" "$TC"

# Install the frontend and its swift-syntax libraries. The usr/bin tools that are symlinks to swift-frontend
# (swift-legacy-driver, swift-symbolgraph-extract, ...) follow automatically.
HOSTDIR=lib/swift/host
LIBS=("$BUILD/$HOSTDIR/compiler"/lib_CompilerSwift*.dylib)
[ -f "${LIBS[0]}" ] || { echo "error: no lib_CompilerSwift*.dylib in $BUILD/$HOSTDIR/compiler" >&2; exit 1; }
CD="$TC/usr/$HOSTDIR/compiler-hyp3r"
mkdir "$CD"
cp "${LIBS[@]}" "$CD/"
F="$TC/usr/bin/swift-frontend"
cp "$FRONTEND" "$F"
chmod 755 "$F" "$CD"/*.dylib
install_name_tool -rpath "@executable_path/../$HOSTDIR/compiler" "@executable_path/../$HOSTDIR/compiler-hyp3r" "$F"
for b in "$F" "$CD"/*.dylib; do
  strip -S "$b"
  codesign -f -s - -i "$(basename "$b" .dylib)" "$b"
  codesign --verify --strict "$b"
done
# Every library the frontend needs must resolve inside the new directory.
for l in $(otool -L "$F" | sed -n 's#^[[:space:]]*@rpath/\(lib_Compiler[^ ]*\).*#\1#p'); do
  [ -f "$CD/$l" ] || { echo "error: $l missing in $CD" >&2; exit 1; }
done

P="$TC/Info.plist"
plutil -replace CFBundleIdentifier -string "$ID" "$P"
plutil -replace DisplayName -string "Swift ${SWIFT_VERSION} ${RELEASE_ID#*-} (${SUFFIX})" "$P"
plutil -replace ShortDisplayName -string "Swift ${SWIFT_VERSION} ${RELEASE_ID#*-}" "$P"
plutil -replace Version -string "$VERSION" "$P"
plutil -replace CreatedDate -date "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$P"
plutil -replace Aliases -array "$P"
plutil -lint "$P"

# Reseal the bundle (Info.plist and the replaced file changed); nested code keeps its own signatures.
codesign -f -s - "$TC"
codesign --verify --strict "$TC"

plutil -p "$P"
"$F" -print-target-info | grep -E '"(compilerVersion|swiftCompilerTag)"'
shasum -a 256 "$F" "$CD"/*.dylib
echo "assembled $TC"
