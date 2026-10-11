#!/bin/bash
# Usage: assemble-toolchain.sh <base.xctoolchain> <swift build dir> <out-dir> <release-id> <version> <display-suffix> \
#          [<libswiftCompatibilitySpan.dylib>]
#   e.g. assemble-toolchain.sh ~/Library/Developer/Toolchains/swift-6.4.0-RELEASE.xctoolchain \
#          <build-script build>/swift-macosx-arm64 build/macos-toolchain 6.4.0-hyp3r.6 6.4.20261011106 "SC-0004" \
#          <build-script build>/swift-span-macosx/out/libswiftCompatibilitySpan.dylib
# Produces <out-dir>/swift-<release-id>.xctoolchain: a copy of the official swift.org toolchain with
#   - usr/bin/swift-frontend replaced. The new frontend runs against the toolchain's official
#     usr/lib/swift/host/compiler/lib_CompilerSwift*.dylib (same rpaths as the official frontend);
#   - usr/bin/ld added: ld-shim.sh, which runs the active developer directory's linker (swift.org toolchains
#     ship no ld, which breaks Xcode's package-manifest linking, see ld-shim.sh);
#   - optionally usr/lib/swift-6.2/macosx/libswiftCompatibilitySpan.dylib replaced by a build from
#     build-span-shim.sh (same sources and exports, current_version stamped instead of 0.0.0, plus an arm64e
#     slice like Xcode's copy).
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
[ $# -eq 6 ] || [ $# -eq 7 ] || { sed -n '2,25p' "$0"; exit 2; }
BASE="$(cd "$1" && pwd)"; BUILD="$(cd "$2" && pwd)"; mkdir -p "$3"; OUT="$(cd "$3" && pwd)"
RELEASE_ID="$4"; VERSION="$5"; SUFFIX="$6"; SPAN="${7:-}"
LD_SHIM="$(cd "$(dirname "$0")" && pwd)/ld-shim.sh"
SWIFT_VERSION="${RELEASE_ID%%-*}"
ID="dev.hyp3r.swift.${RELEASE_ID}"
TC="$OUT/swift-${RELEASE_ID}.xctoolchain"
HOSTLIB=lib/swift/host/compiler

[ -f "$BASE/Info.plist" ] && [ -x "$BASE/usr/bin/swift-frontend" ] || { echo "error: $BASE is not a toolchain" >&2; exit 1; }
[ -x "$BUILD/bin/swift-frontend" ] || { echo "error: no $BUILD/bin/swift-frontend; run ninja swift-frontend" >&2; exit 1; }
[ ! -e "$TC" ] || { echo "error: $TC exists; remove it first" >&2; exit 1; }
[ -f "$LD_SHIM" ] || { echo "error: no $LD_SHIM" >&2; exit 1; }
[ ! -e "$BASE/usr/bin/ld" ] || { echo "error: base toolchain already has usr/bin/ld; review the shim" >&2; exit 1; }
[ -z "$SPAN" ] || [ -f "$SPAN" ] || { echo "error: no $SPAN" >&2; exit 1; }

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

# usr/bin/ld shim (see ld-shim.sh). Must resolve to a real linker, not to itself.
install -m 755 "$LD_SHIM" "$TC/usr/bin/ld"
real_ld=$(env -u TOOLCHAINS /usr/bin/xcrun --find ld)
"$TC/usr/bin/ld" -v 2>&1 | head -1
[ "$(env -i "$TC/usr/bin/ld" -v 2>&1 | head -1)" = "$("$real_ld" -v 2>&1 | head -1)" ] || { echo "error: ld shim does not reach $real_ld" >&2; exit 1; }

# Span back-deployment library with a stamped current_version: same install name and exported symbols as the
# swift.org copy it replaces, for every architecture of that copy. It may add an arm64e slice (Xcode's copy has
# x86_64, arm64 and arm64e; swift.org's lacks arm64e); that slice is checked against the official arm64 slice.
if [ -n "$SPAN" ]; then
  S="$TC/usr/lib/swift-6.2/macosx/libswiftCompatibilitySpan.dylib"
  [ -f "$S" ] || { echo "error: base toolchain has no $S" >&2; exit 1; }
  span_archs=" $(lipo -archs "$SPAN") "
  for a in $(lipo -archs "$S"); do
    case "$span_archs" in *" $a "*) ;; *) echo "error: $SPAN lacks $a" >&2; exit 1 ;; esac
  done
  for a in $span_archs; do
    ref=$a; [ "$a" = arm64e ] && ref=arm64
    case " $(lipo -archs "$S") " in *" $ref "*) ;; *) echo "error: unexpected architecture $a" >&2; exit 1 ;; esac
    [ "$(otool -D -arch "$a" "$SPAN" | tail -1)" = "$(otool -D -arch "$ref" "$S" | tail -1)" ] || { echo "error: install name differs ($a)" >&2; exit 1; }
    cmp -s <(nm -gUj -arch "$a" "$SPAN" | sort) <(nm -gUj -arch "$ref" "$S" | sort) \
      || { echo "error: exported symbols differ ($a vs official $ref)" >&2; exit 1; }
  done
  cp "$SPAN" "$S"; chmod 755 "$S"
  strip -S "$S"
  codesign -f -s - -i libswiftCompatibilitySpan "$S"
  codesign --verify --strict "$S"
  echo "Span shim: $(otool -l -arch arm64 "$S" | awk '/LC_ID_DYLIB/{f=1} f&&/current version/{print "current_version "$3; exit}')"
fi

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
shasum -a 256 "$F" "$TC/usr/bin/ld" ${SPAN:+"$TC/usr/lib/swift-6.2/macosx/libswiftCompatibilitySpan.dylib"}
echo "assembled $TC"
