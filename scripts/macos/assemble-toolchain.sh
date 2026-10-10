#!/bin/bash
# Usage: assemble-toolchain.sh <base.xctoolchain> <swift build dir> <out-dir> <release-id> <version> <display-suffix>
#   e.g. assemble-toolchain.sh ~/Library/Developer/Toolchains/swift-6.4.0-RELEASE.xctoolchain \
#          <build-script build>/swift-macosx-arm64 build/macos-toolchain 6.4.0-hyp3r.4 6.4.20261010104 "SC-0004"
# Produces <out-dir>/swift-<release-id>.xctoolchain: a copy of the official swift.org toolchain in which only
# usr/bin/swift-frontend is replaced. The new frontend runs against the toolchain's official
# usr/lib/swift/host/compiler/lib_CompilerSwift*.dylib (same rpaths as the official frontend).
#
# Relink: the frontend's final link is redone (from `ninja -t commands`) against the base toolchain's
# lib_CompilerSwift*.dylib instead of the ones in the build tree. The code is unchanged; only the binding of
# imports changes. Why: the build tree's swift-syntax libraries come from the same source tag, but a newer host
# Swift compiler emits some of their exports as weak definitions (e.g. SyntaxRewriter.visitImpl). ld then
# records a <weak-def-coalesce> bind in the frontend, which dyld cannot satisfy from the official dylibs, where
# the same symbol is a regular export ("Symbol not found ... Expected as weak-def export").
#
# The result is stripped like build-script's darwin-install-extract-symbols does and signed ad hoc. The bundle
# gets the identifier dev.hyp3r.swift.<release-id>, so it installs next to the official one and is selected with
# TOOLCHAINS=dev.hyp3r.swift.<release-id>; Aliases is emptied, the toolchain never claims "swift". The base
# toolchain is only read. The bundle seal is redone ad hoc; untouched binaries keep their Developer ID signatures.
set -euo pipefail
[ $# -eq 6 ] || { sed -n '2,20p' "$0"; exit 2; }
BASE="$(cd "$1" && pwd)"; BUILD="$(cd "$2" && pwd)"; mkdir -p "$3"; OUT="$(cd "$3" && pwd)"
RELEASE_ID="$4"; VERSION="$5"; SUFFIX="$6"
SWIFT_VERSION="${RELEASE_ID%%-*}"
ID="dev.hyp3r.swift.${RELEASE_ID}"
TC="$OUT/swift-${RELEASE_ID}.xctoolchain"
HOSTLIB=lib/swift/host/compiler

[ -f "$BASE/Info.plist" ] && [ -x "$BASE/usr/bin/swift-frontend" ] || { echo "error: $BASE is not a toolchain" >&2; exit 1; }
[ -x "$BUILD/bin/swift-frontend" ] || { echo "error: no $BUILD/bin/swift-frontend; run ninja swift-frontend" >&2; exit 1; }
[ ! -e "$TC" ] || { echo "error: $TC exists; remove it first" >&2; exit 1; }

# Relink against the official compiler libraries (see above).
RELINKED="$BUILD/bin/swift-frontend.relinked"
cmd=$(cd "$BUILD" && ninja -t commands bin/swift-frontend | tail -1)
grep -q -- " -o bin/swift-frontend " <<<"$cmd" || { echo "error: unexpected link command" >&2; exit 1; }
n=$(grep -o " $HOSTLIB/lib_CompilerSwift[A-Za-z]*\.dylib" <<<"$cmd" | wc -l)
[ "$n" -gt 0 ] || { echo "error: link command has no $HOSTLIB/lib_CompilerSwift*.dylib" >&2; exit 1; }
cmd=$(sed -e "s# $HOSTLIB/lib_CompilerSwift\([A-Za-z]*\)\.dylib# $BASE/usr/$HOSTLIB/lib_CompilerSwift\1.dylib#g" \
          -e "s# -o bin/swift-frontend # -o bin/swift-frontend.relinked #" <<<"$cmd")
for l in $(grep -o "$BASE/usr/$HOSTLIB/lib_CompilerSwift[A-Za-z]*\.dylib" <<<"$cmd"); do
  [ -f "$l" ] || { echo "error: base toolchain lacks $l" >&2; exit 1; }
done
(cd "$BUILD" && rm -f "$RELINKED" && nice -n 10 bash -c "$cmd")
echo "relinked against $n official libraries"

base_archs=$(lipo -archs "$BASE/usr/bin/swift-frontend"); new_archs=$(lipo -archs "$RELINKED")
echo "base swift-frontend: $base_archs; new: $new_archs"
[ "$base_archs" = "$new_archs" ] || echo "warning: the toolchain will only run on: $new_archs"

ditto "$BASE" "$TC"

# Install the frontend. The usr/bin tools that are symlinks to swift-frontend (swift-legacy-driver,
# swift-symbolgraph-extract, ...) follow automatically.
F="$TC/usr/bin/swift-frontend"
mv "$RELINKED" "$F"
chmod 755 "$F"
strip -S "$F"
codesign -f -s - -i swift-frontend "$F"
codesign --verify --strict "$F"

# No import may still need weak-def coalescing from the compiler libraries, and the rpaths must match the
# official frontend's.
if dyld_info -fixups "$F" | grep -q "<weak-def-coalesce>/_\$s[0-9]*_Compiler"; then
  echo "error: weak-def binds to _Compiler* symbols remain" >&2; exit 1
fi
rp() { otool -l -arch arm64 "$1" | awk '/LC_RPATH/{getline; getline; print $2}'; }
[ "$(rp "$F")" = "$(rp "$BASE/usr/bin/swift-frontend")" ] || { echo "error: rpaths differ from the official frontend" >&2; exit 1; }
# Load check: every lib_Compiler* image must come from the toolchain's own host/compiler directory.
loaded=$(DYLD_PRINT_LIBRARIES=1 "$F" -version 2>&1 >/dev/null | sed -n 's#.*> \(.*lib_Compiler[^ ]*\)$#\1#p')
[ -n "$loaded" ] && ! grep -v "^$TC/usr/$HOSTLIB/" <<<"$loaded" || { echo "error: unexpected compiler library load" >&2; exit 1; }
echo "loads $(wc -l <<<"$loaded" | tr -d ' ') official compiler libraries from usr/$HOSTLIB"

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
shasum -a 256 "$F"
echo "assembled $TC"
