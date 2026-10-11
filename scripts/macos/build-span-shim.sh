#!/bin/bash
# Usage: build-span-shim.sh <build-script log> <build-script build root> <version>
#   e.g. build-span-shim.sh build/sc0004-build-6.4-release.log repos/swift-project/build/Ninja-Release 6.4.0
# Builds the macOS Span back-deployment library (libswiftCompatibilitySpan.dylib, arm64 + x86_64) with the
# Mach-O current_version stamped as <version>, using the compiler already built in <root>/swift-macosx-arm64.
#
# Why: Xcode embeds usr/lib/swift-6.2/macosx/libswiftCompatibilitySpan.dylib from the selected toolchain into
# apps that deploy below macOS 26 and use Span. swift.org builds it without SWIFT_COMPILER_VERSION, so its
# LC_ID_DYLIB current_version is 0.0.0 (Xcode's copy: 6.4.0). The library is compiled with -parse-stdlib from
# the Span sources only, so it needs no stdlib build: a stdlib-only configure (derived from the swift configure
# recorded in the build-script log, plus the overrides below) and the one ninja target, a few minutes.
# SWIFT_COMPILER_VERSION is set only in this separate build directory; the compiler is not rebuilt.
# Output: <root>/swift-span-macosx/lib/swift-6.2/macosx/libswiftCompatibilitySpan.dylib
set -euo pipefail
[ $# -eq 3 ] || { sed -n '2,13p' "$0"; exit 2; }
LOG="$1"; ROOT="$(cd "$2" && pwd)"; VERSION="$3"
D="$ROOT/swift-span-macosx"
mkdir -p "$D"
python3 - "$LOG" "$ROOT" "$VERSION" > "$D/configure.sh" <<'EOF'
import shlex, sys
log, root, version = sys.argv[1:]
lines = [l for l in open(log, errors='replace').read().split('\n')
         if l.startswith('+ env ') and ' -G Ninja ' in l and l.rstrip().endswith('/swift')
         and f'{root}/swift-macosx-arm64' in l]
if not lines:
    sys.exit('error: no swift configure command for swift-macosx-arm64 in the log')
args = shlex.split(lines[-1][2:])
over = ['-DSWIFT_INCLUDE_TOOLS:BOOL=FALSE', '-DSWIFT_BUILD_DYNAMIC_STDLIB:BOOL=TRUE',
        '-DSWIFT_INCLUDE_TESTS:BOOL=FALSE', '-DSWIFT_BUILD_REMOTE_MIRROR:BOOL=FALSE',
        '-DSWIFT_DARWIN_SUPPORTED_ARCHS:STRING=arm64;x86_64',
        f'-DSWIFT_NATIVE_SWIFT_TOOLS_PATH:STRING={root}/swift-macosx-arm64/bin',
        f'-DSWIFT_NATIVE_LLVM_TOOLS_PATH:STRING={root}/llvm-macosx-arm64/bin',
        f'-DSWIFT_NATIVE_CLANG_TOOLS_PATH:STRING={root}/llvm-macosx-arm64/bin',
        f'-DSWIFT_COMPILER_VERSION={version}']
print('#!/bin/bash\nset -euo pipefail\ncd "$(dirname "$0")"')
print(' '.join(shlex.quote(a) for a in args[:-1] + over + [args[-1]]))
EOF
chmod +x "$D/configure.sh"
(cd "$D" && nice -n 10 ./configure.sh > configure.log 2>&1) || { tail -20 "$D/configure.log"; exit 1; }
(cd "$D" && nice -n 10 ninja -j6 swiftCompatibilitySpan-macosx > build.log 2>&1) || { tail -20 "$D/build.log"; exit 1; }
OUT="$D/lib/swift-6.2/macosx/libswiftCompatibilitySpan.dylib"
for a in arm64 x86_64; do
  v=$(otool -l -arch "$a" "$OUT" | awk '/LC_ID_DYLIB/{f=1} f&&/current version/{print $3; exit}')
  [ "$v" = "$VERSION" ] || { echo "error: $a current_version is $v, expected $VERSION" >&2; exit 1; }
done
echo "built $OUT (current_version $VERSION, $(lipo -archs "$OUT"))"
