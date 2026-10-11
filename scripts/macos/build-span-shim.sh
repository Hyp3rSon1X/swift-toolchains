#!/bin/bash
# Usage: build-span-shim.sh <build-script log> <build-script build root> <version>
#   e.g. build-span-shim.sh build/sc0004-build-6.4-release.log repos/swift-project/build/Ninja-Release 6.4.0
# Builds the macOS Span back-deployment library (libswiftCompatibilitySpan.dylib, x86_64 + arm64 + arm64e) with
# the Mach-O current_version stamped as <version>, using the compiler already built in <root>/swift-macosx-arm64.
#
# Why: Xcode embeds usr/lib/swift-6.2/macosx/libswiftCompatibilitySpan.dylib from the selected toolchain into
# apps that deploy below macOS 26 and use Span. swift.org builds it without SWIFT_COMPILER_VERSION, so its
# LC_ID_DYLIB current_version is 0.0.0 (Xcode's copy: 6.4.0). The library is compiled with -parse-stdlib from
# the Span sources only, so it needs no stdlib build: a stdlib-only configure (derived from the swift configure
# recorded in the build-script log, plus the overrides below) and the one ninja target, a few minutes.
# SWIFT_COMPILER_VERSION is set only in this separate build directory; the compiler is not rebuilt.
#
# arm64e: Xcode's copy has x86_64, arm64 and arm64e slices, swift.org's only x86_64 and arm64 (the CMake build
# does not offer arm64e for macOS), and some consumers check slice parity. The arm64e slice is built with the
# arm64 slice's own ninja compile and link commands, retargeted to arm64e-apple-macosx; the link uses a stub of
# the just-built arm64 libswiftCore with an arm64e target added, so the libswiftCore load command records the
# same versions as the other slices (compatibility 1.0.0, current 6.4.0). Checks: ptrauth ABI version 0 for
# userspace (as Apple's slice), and the same install name, exports and imports as the arm64 slice.
# Output: <root>/swift-span-macosx/out/libswiftCompatibilitySpan.dylib (universal; the ninja-built two-slice
# library stays in lib/swift-6.2/macosx/)
set -euo pipefail
[ $# -eq 3 ] || { sed -n '2,22p' "$0"; exit 2; }
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
L="$D/lib/swift-6.2/macosx"
for a in arm64 x86_64; do
  v=$(otool -l -arch "$a" "$L/libswiftCompatibilitySpan.dylib" | awk '/LC_ID_DYLIB/{f=1} f&&/current version/{print $3; exit}')
  [ "$v" = "$VERSION" ] || { echo "error: $a current_version is $v, expected $VERSION" >&2; exit 1; }
done

# arm64e slice
mkdir -p "$D/arm64e-stub" "$D/out"
xcrun tapi stubify "$D/lib/swift/macosx/arm64/libswiftCore.dylib" -o "$D/arm64e-stub/libswiftCore.tbd"
python3 - "$D/arm64e-stub/libswiftCore.tbd" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
t = d['main_library']['target_info']
assert [x['target'] for x in t] == ['arm64-macos'], t
t.append({'target': 'arm64e-macos', 'min_deployment': '11'})
json.dump(d, open(sys.argv[1], 'w'))
EOF
python3 - "$D" > "$D/arm64e.sh" <<'EOF'
import re, subprocess, sys
d = sys.argv[1]
def cmd(target):
    return subprocess.run(['ninja', '-C', d, '-t', 'commands', '-s', target], check=True,
                          capture_output=True, text=True).stdout.strip()
def retarget(c):
    c = re.sub(r'\barm64-apple-macosx', 'arm64e-apple-macosx', c)
    c = re.sub(r'/OSX/arm64(?=[/\s]|$)', '/OSX/arm64e', c)
    c = c.replace('lib/swift-6.2/macosx/arm64/', 'lib/swift-6.2/macosx/arm64e/')
    return c.replace(' -arch arm64 ', ' -arch arm64e ')
compile_ = retarget(cmd('stdlib/toolchain/CompatibilitySpan/OSX/arm64/CompatibilitySpan.o'))
link = retarget(cmd('lib/swift-6.2/macosx/arm64/libswiftCompatibilitySpan.dylib'))
core = ' lib/swift/macosx/arm64/libswiftCore.dylib'
assert core in link, 'link command no longer names libswiftCore'
link = link.replace(core, f' {d}/arm64e-stub/libswiftCore.tbd')
for c in (compile_, link):
    assert 'arm64e-apple-macosx' in c and 'arm64-apple-macosx' not in c, c
print('#!/bin/bash\nset -euo pipefail\ncd "$(dirname "$0")"')
print('mkdir -p stdlib/toolchain/CompatibilitySpan/OSX/arm64e lib/swift-6.2/macosx/arm64e')
print(f'( {compile_} )\n( {link} )')
EOF
(cd "$D" && nice -n 10 bash arm64e.sh > arm64e.log 2>&1) || { tail -20 "$D/arm64e.log"; exit 1; }
E="$L/arm64e/libswiftCompatibilitySpan.dylib"
# cpusubtype ARM64E with capability bits USR00: userspace, ptrauth ABI version 0 (Xcode's slice has the same).
[ "$(otool -hv "$E" | tail -1 | awk '{print $2, $3, $4}')" = "ARM64 E USR00" ] \
  || { echo "error: arm64e slice is not ptrauth ABI version 0 (userspace)" >&2; otool -hv "$E" >&2; exit 1; }
# dyld_stub_binder: our arm64 slice (like swift.org's) uses lazy binding, arm64e always uses chained fixups.
facts() { otool -L "$1" | tail -n +2; nm -gUj "$1"; echo; nm -uj "$1" | grep -vx dyld_stub_binder; }
[ "$(facts "$L/arm64/libswiftCompatibilitySpan.dylib")" = "$(facts "$E")" ] \
  || { echo "error: arm64e and arm64 slices differ in dependencies, exports or imports" >&2; exit 1; }

# Universal library, signed like the ninja-built one.
OUT="$D/out/libswiftCompatibilitySpan.dylib"
lipo -create "$L/x86_64/libswiftCompatibilitySpan.dylib" "$L/arm64/libswiftCompatibilitySpan.dylib" "$E" -output "$OUT"
codesign -f -s - "$OUT"
[ "$(lipo -archs "$OUT")" = "x86_64 arm64 arm64e" ] || { echo "error: $OUT has $(lipo -archs "$OUT")" >&2; exit 1; }
echo "built $OUT (current_version $VERSION, $(lipo -archs "$OUT"))"
