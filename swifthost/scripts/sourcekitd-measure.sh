#!/bin/bash
# Measurement only (SPIKE-SOURCEKITD-001): what the in-process sourcekitd adds to an app.
#
#   sourcekitd-measure.sh <slice> <base-object>
#
# Needs a slice built with `SOURCEKIT=1 build.sh slice <slice>` in $BUILD (same SRC/BUILD as there).
# <base-object> is a SwiftHostCompiler.o without SourceKit (e.g. the released -3 one).
#
# 1. Prelinks sourcekitd alone (its objects and every archive on its link line, compiler
#    included) into $BUILD/out/<slice>/sourcekitdInProc.o, exporting only its 191 C symbols.
# 2. Prints sizes for four variants: base, shared (base + sourcekitd in one object, from
#    build.sh), separate (sourcekitdInProc.o alone), base + separate (both objects):
#    object bytes, `libtool -static` archive bytes, zip bytes of that archive, and a tiny
#    -dead_strip app referencing the exported entry points (file bytes and segment sizes).
#    macosx only for the app link (it runs nothing).
set -euo pipefail
S=${1:?slice} BASE=$(cd "$(dirname "${2:?base object}")" && pwd)/$(basename "$2")
SRC=$(cd "${SRC:-src}" && pwd)
BUILD=$(cd "${BUILD:-build}" && pwd)
case $S in
  iphoneos)        TRIPLE=arm64-apple-ios27.0;           PLAT=ios;;
  iphonesimulator) TRIPLE=arm64-apple-ios27.0-simulator; PLAT=ios-simulator;;
  macosx)          TRIPLE=arm64-apple-macosx27.0;        PLAT=macos;;
  *) echo "unknown slice: $S" >&2; exit 2;;
esac
SYSROOT=$(xcrun --sdk "$S" --show-sdk-path)
SDKVER=$(xcrun --sdk "$S" --show-sdk-version)
TOOLCHAIN=$(dirname "$(dirname "$(xcrun -f swiftc)")")
B=$BUILD/swift-$S O=$BUILD/out/$S M=$BUILD/measure/$S
rm -rf "$M" && mkdir -p "$M"

# 1. The separate object.
objects=() archives=()
while read -r a; do
  case $a in /*) ;; *) a=$B/$a;; esac
  case $a in *.o) objects+=("$a");; *) archives+=("$a");; esac
done < <(cd "$B" && ninja -t commands lib/libsourcekitdInProc.dylib | tail -1 | tr ' ' '\n' | grep -E '\.(a|o)$' |
  awk '!s[$0]++')
sed 's/^/_/' "$SRC/swift/tools/SourceKit/tools/sourcekitd/bin/InProc/sourcekitdInProc.exports" > "$O/sourcekitd-exports.txt"
undefined=() ignored=()
while read -r a; do undefined+=(-u "$a"); done < "$O/sourcekitd-exports.txt"
for a in "${archives[@]}"; do
  a=${a##*/} a=${a#lib}
  ignored+=(-ignore_auto_link_library "${a%.a}")
done
xcrun ld -r -arch arm64 -platform_version $PLAT 27.0 "$SDKVER" -exported_symbols_list "$O/sourcekitd-exports.txt" \
  "${ignored[@]}" "${undefined[@]}" -o "$O/sourcekitdInProc.o" "${objects[@]}" "${archives[@]}"
exported=$(xcrun nm -gU "$O/sourcekitdInProc.o" | awk '{print $3}' | sort | tr '\n' ' ')
if [[ $exported != "$(sort "$O/sourcekitd-exports.txt" | tr '\n' ' ')" ]]; then
  echo "unexpected exported symbols: $exported" >&2
  exit 1
fi

# 2. Sizes.
cat > "$M/app.c" <<'EOF'
#include <stdio.h>
/* Only the addresses are used: the app keeps the entry points alive and runs nothing. */
#ifdef USE_SWIFTHOST
extern void swifthost_swift_frontend(void);
#endif
#ifdef USE_SOURCEKITD
extern void sourcekitd_initialize(void);
extern void sourcekitd_send_request_sync(void);
#endif
int main(void) {
  void *volatile p[] = {
#ifdef USE_SWIFTHOST
    (void *)swifthost_swift_frontend,
#endif
#ifdef USE_SOURCEKITD
    (void *)sourcekitd_initialize, (void *)sourcekitd_send_request_sync,
#endif
    0};
  printf("%p\n", p[0]);
  return 0;
}
EOF
segment() { xcrun size -m "$1" | awk -v s="Segment $2:" 'index($0,s)==1{print $3; f=1} END{if(!f) print 0}'; }
printf '| variant | .o bytes | .a bytes | .a zip bytes | app bytes | app __TEXT | app __DATA_CONST | app __DATA | app __LINKEDIT |\n|-|-|-|-|-|-|-|-|-|\n'
variant() { # name defines objects...
  local name=$1 defines=$2; shift 2
  local d=$M/$name obytes=0 o
  mkdir -p "$d"
  for o in "$@"; do obytes=$((obytes + $(stat -f %z "$o"))); done
  xcrun libtool -static -o "$d/lib.a" "$@"
  (cd "$d" && ditto -c -k --norsrc --noextattr --keepParent lib.a lib.a.zip)
  # shellcheck disable=SC2086 # defines is a word list
  xcrun clang -target $TRIPLE -isysroot "$SYSROOT" -O1 $defines -c "$M/app.c" -o "$d/app.o"
  xcrun clang++ -target $TRIPLE -isysroot "$SYSROOT" "$d/app.o" "$d/lib.a" \
    -L"$SYSROOT/usr/lib/swift" -L"$TOOLCHAIN/lib/swift/$S" -dead_strip -o "$d/app"
  printf '| %s | %s | %s | %s | %s | %s | %s | %s | %s |\n' "$name" "$obytes" "$(stat -f %z "$d/lib.a")" \
    "$(stat -f %z "$d/lib.a.zip")" "$(stat -f %z "$d/app")" "$(segment "$d/app" __TEXT)" \
    "$(segment "$d/app" __DATA_CONST)" "$(segment "$d/app" __DATA)" "$(segment "$d/app" __LINKEDIT)"
}
variant base "-DUSE_SWIFTHOST" "$BASE"
variant shared "-DUSE_SWIFTHOST -DUSE_SOURCEKITD" "$O/SwiftHostCompiler.o"
variant separate "-DUSE_SOURCEKITD" "$O/sourcekitdInProc.o"
variant base+separate "-DUSE_SWIFTHOST -DUSE_SOURCEKITD" "$BASE" "$O/sourcekitdInProc.o"
