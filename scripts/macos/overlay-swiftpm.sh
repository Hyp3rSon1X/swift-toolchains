#!/bin/bash
# Usage: overlay-swiftpm.sh <full .xctoolchain from assemble-toolchain.sh> <SwiftPM-only .xctoolchain (extracted from a swiftpm-<id>-macos-<arch> tarball)>
#   Run after assemble-toolchain.sh and before package-toolchain.sh.
# Replaces the SwiftPM of a swift.org based toolchain with the one of a SwiftPM-only toolchain (build-swiftpm-toolchain.sh):
#   usr/bin/swift-package and its multi-call symlinks (swift-build, swift-test, swift-run, swift-sdk, ...)
#   usr/libexec/swift/pm/swiftpm-testing-helper   (the swift.org location; UserToolchain.getSwiftTestingHelper searches the
#                                                  compiler's usr/bin first, then usr/libexec/swift/pm; no copy may exist in usr/bin)
#   usr/lib/swift/pm/{ManifestAPI,PluginAPI}      (dylibs plus textual .swiftinterface files; swift.org ships binary .swiftmodule
#                                                  files for arm64 and x86_64, which are removed)
#   usr/share/pm                                   (SwiftBuild/SwiftPM/crypto resource bundles and config.json)
# Everything else stays as swift.org ships it. Not SwiftPM-owned and kept on purpose: usr/bin/swift-build-tool and
# usr/lib/swift/macosx/libllbuildSwift.dylib (llbuild, which the new swift-package does not use: it links its own copy).
# Before anything is deleted, no other Mach-O of the toolchain may depend on a SwiftPM file, and every swift.org file that has
# no counterpart in the new SwiftPM must be on the short list below, otherwise the script stops. Finally the bundle seal is
# redone ad hoc, like assemble-toolchain.sh does (the replaced Mach-Os keep their own ad hoc signatures).
set -euo pipefail
[ $# -eq 2 ] || { sed -n '2,3p' "$0"; exit 2; }
TC="$(cd "$1" && pwd)"; SRC="$(cd "$2" && pwd)"
for d in "$TC" "$SRC"; do [ -f "$d/Info.plist" ] || { echo "error: $d is not a toolchain" >&2; exit 1; }; done
[ -x "$SRC/usr/bin/swift-package" ] || { echo "error: $SRC has no usr/bin/swift-package" >&2; exit 1; }
[ -x "$SRC/usr/bin/swiftpm-testing-helper" ] || { echo "error: $SRC has no usr/bin/swiftpm-testing-helper" >&2; exit 1; }
[ -x "$TC/usr/bin/swift-package" ] && [ -x "$TC/usr/libexec/swift/pm/swiftpm-testing-helper" ] \
  || { echo "error: $TC does not have the swift.org SwiftPM layout" >&2; exit 1; }
[ ! -e "$TC/usr/bin/swiftpm-testing-helper" ] || { echo "error: stale $TC/usr/bin/swiftpm-testing-helper" >&2; exit 1; }
for m in ManifestAPI/PackageDescription ManifestAPI/CompilerPluginSupport PluginAPI/PackagePlugin; do
  ls "$SRC/usr/lib/swift/pm/$m.swiftmodule/"*.swiftinterface >/dev/null 2>&1 \
    || { echo "error: $SRC lacks the textual interface of $m" >&2; exit 1; }
done
[ -f "$SRC/usr/share/pm/config.json" ] || { echo "error: $SRC lacks usr/share/pm/config.json" >&2; exit 1; }

# 1. Multi-call tool names: every usr/bin entry of the new SwiftPM that is a symlink to swift-package.
LINKS=$(cd "$SRC/usr/bin" && for f in *; do if [ -L "$f" ] && [ "$(readlink "$f")" = swift-package ]; then echo "$f"; fi; done | sort)
[ -n "$LINKS" ] || { echo "error: no swift-package symlinks in $SRC/usr/bin" >&2; exit 1; }

# 2. Files that swift.org has and the new SwiftPM does not: only these known ones may remain unmatched.
#    - usr/share/pm/SwiftPM_SBOMModelTests.bundle: test data of SwiftPM's own test target, not used at run time
#    - usr/lib/swift/pm/**: the binary .swiftmodule/.abi.json files and the x86_64 slices (replaced by textual interfaces)
old_only() { # <dir under usr>
  (cd "$TC/usr/$1" && find . | sort) > "$W/old.txt"; (cd "$SRC/usr/$1" && find . | sort) > "$W/new.txt"; comm -23 "$W/old.txt" "$W/new.txt"
}
W="$(mktemp -d "$(dirname "$TC")/.overlay-swiftpm.XXXXXX")"; trap 'rm -rf "$W"' EXIT
unexpected=$(old_only share/pm | grep -v '^\./SwiftPM_SBOMModelTests\.bundle' || true)
[ -z "$unexpected" ] || { echo "error: swift.org files in usr/share/pm without a counterpart:" >&2; echo "$unexpected" >&2; exit 1; }
unexpected=$(old_only lib/swift/pm | grep -vE '\.(swiftmodule|abi\.json)$|/x86_64-apple-macos\.swiftdoc$' || true)
[ -z "$unexpected" ] || { echo "error: swift.org files in usr/lib/swift/pm without a counterpart:" >&2; echo "$unexpected" >&2; exit 1; }

# 3. No other Mach-O of the toolchain may be linked against, or name, a file that goes away.
dep=0
while IFS= read -r f; do
  case "$f" in "$TC/usr/lib/swift/pm/"*|"$TC/usr/bin/swift-package") continue;; esac
  if otool -L "$f" 2>/dev/null | sed 1d | grep -qE 'libPackageDescription|libPackagePlugin|SwiftPM'; then echo "error: $f depends on SwiftPM libraries" >&2; dep=1; fi
done < <(find "$TC/usr/bin" "$TC/usr/lib" "$TC/usr/libexec" -type f ! -name '*.swiftmodule' ! -name '*.swiftdoc' ! -name '*.json' ! -name '*.swiftinterface' -size +1k \
         -exec sh -c 'for f; do file -b "$f" | grep -q Mach-O && echo "$f"; done' sh {} +)
[ "$dep" = 0 ] || exit 1
echo "no other Mach-O links against the SwiftPM libraries"

# 4. Replace.
for l in $LINKS; do rm -f "$TC/usr/bin/$l"; done
rm -f "$TC/usr/bin/swift-package"
cp -p "$SRC/usr/bin/swift-package" "$TC/usr/bin/swift-package"
for l in $LINKS; do cp -PR "$SRC/usr/bin/$l" "$TC/usr/bin/$l"; done
rm -f "$TC/usr/libexec/swift/pm/swiftpm-testing-helper"
cp -p "$SRC/usr/bin/swiftpm-testing-helper" "$TC/usr/libexec/swift/pm/swiftpm-testing-helper"
rm -rf "$TC/usr/lib/swift/pm" "$TC/usr/share/pm"
mkdir -p "$TC/usr/lib/swift"
ditto "$SRC/usr/lib/swift/pm" "$TC/usr/lib/swift/pm"
ditto "$SRC/usr/share/pm" "$TC/usr/share/pm"
echo "replaced: swift-package + $(wc -w <<<"$LINKS" | tr -d ' ') links, libexec/swift/pm/swiftpm-testing-helper, lib/swift/pm, share/pm"

# 5. Checks: the helper the new SwiftPM finds is ours, nothing stale on its search path, signatures hold.
[ ! -e "$TC/usr/bin/swiftpm-testing-helper" ] || { echo "error: stray usr/bin/swiftpm-testing-helper" >&2; exit 1; }
cmp -s "$SRC/usr/bin/swiftpm-testing-helper" "$TC/usr/libexec/swift/pm/swiftpm-testing-helper" || { echo "error: helper differs" >&2; exit 1; }
cmp -s "$SRC/usr/bin/swift-package" "$TC/usr/bin/swift-package" || { echo "error: swift-package differs" >&2; exit 1; }
for f in "$TC/usr/bin/swift-package" "$TC/usr/libexec/swift/pm/swiftpm-testing-helper" "$TC"/usr/lib/swift/pm/*/lib*.dylib; do codesign --verify --strict "$f"; done
for a in "$TC"/usr/lib/swift/pm/*/*.swiftmodule; do ls "$a"/*.swiftinterface >/dev/null; done
! find "$TC/usr/lib/swift/pm" -name '*.swiftmodule' -type f | grep -q . || { echo "error: a binary .swiftmodule remains" >&2; exit 1; }

codesign -f -s - "$TC"
codesign --verify --deep --strict "$TC"
echo "overlaid SwiftPM of $(basename "$SRC") onto $(basename "$TC"); codesign --deep --strict ok"
shasum -a 256 "$TC/usr/bin/swift-package" "$TC/usr/libexec/swift/pm/swiftpm-testing-helper"
