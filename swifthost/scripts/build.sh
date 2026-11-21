#!/bin/bash
# Builds SwiftHostCompiler from source on an Apple silicon Mac with Xcode 27.2.
#
#   build.sh sources                 clone the sources at the pinned commits into $SRC
#   build.sh native                  host llvm-tblgen and clang-tblgen (into $BUILD/native)
#   build.sh slice <slice>           LLVM/clang/lld + cmark + Swift libraries for one slice, then the
#                                    prelinked object $BUILD/out/<slice>/SwiftHostCompiler.o
#   build.sh package <release-id>    SwiftHostCompiler.xcframework (+ zip, SHA256SUMS, sizes.txt)
#                                    from all three slices, in $BUILD/dist/<release-id>
#
# For a release, set VERSION to the release id for the slice steps and pass the same id to package:
#   export VERSION=swifthost-6.4.2-3; build.sh slice iphoneos; ...; build.sh package "$VERSION"
#
# <slice> is iphoneos, iphonesimulator or macosx (all arm64, deployment target 27.0).
# Every step is re-runnable: cmake and ninja continue where they stopped.
#
# Environment:
#   SRC      source checkouts (default ./src): swift, llvm-project, cmark, swift-experimental-string-processing,
#            swift-syntax
#   BUILD    build root (default ./build); a slice needs about 1 GB
#   NATIVE   directory with host llvm-tblgen and clang-tblgen (default $BUILD/native/bin)
#   JOBS     parallel jobs (default: all cores)
#   VERSION  version label compiled into the compiler and returned by swifthost_version()
#            (default swifthost-6.4.2-3, whatever id package gets); set it to the release id
#   SOURCEKIT 1: also build the in-process sourcekitd (the C API of sourcekitd.h, no XPC service)
#            and prelink it into the same object, which then also exports its 191 C entry points
#            (default 0: the compiler alone, as released in swifthost-6.4.2-3)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC=$(mkdir -p "${SRC:-src}" && cd "${SRC:-src}" && pwd)
BUILD=$(mkdir -p "${BUILD:-build}" && cd "${BUILD:-build}" && pwd)
NATIVE=${NATIVE:-$BUILD/native/bin}
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}
VERSION=${VERSION:-swifthost-6.4.2-3}
SOURCEKIT=${SOURCEKIT:-0}
CC=$(xcrun -f clang)
CXX=$(xcrun -f clang++)

# Pinned sources. swift and llvm-project carry the patches listed in the README.
SWIFT_REPO=https://github.com/Hyp3rSon1X/swift.git
SWIFT_COMMIT=557485911d1b5940b9fa39f9ee42c8dbbfcfb43a
LLVM_REPO=https://github.com/Hyp3rSon1X/llvm-project.git
LLVM_COMMIT=e9a6797a92ff70f31318833c2539a129734ebe9c
CMARK_REPO=https://github.com/swiftlang/swift-cmark.git
CMARK_TAG=swift-6.4.2-RELEASE
STRING_PROCESSING_REPO=https://github.com/swiftlang/swift-experimental-string-processing.git
STRING_PROCESSING_TAG=swift-6.4.2-RELEASE
SWIFT_SYNTAX_REPO=https://github.com/swiftlang/swift-syntax.git
SWIFT_SYNTAX_TAG=swift-6.4.2-RELEASE

slice_vars() {
  case $1 in
    iphoneos)        SYS=iOS;    VARIANT_SDK=IOS;           TRIPLE=arm64-apple-ios27.0;           PLAT=ios;;
    iphonesimulator) SYS=iOS;    VARIANT_SDK=IOS_SIMULATOR; TRIPLE=arm64-apple-ios27.0-simulator; PLAT=ios-simulator;;
    macosx)          SYS=Darwin; VARIANT_SDK=OSX;           TRIPLE=arm64-apple-macosx27.0;        PLAT=macos;;
    *) echo "unknown slice: $1" >&2; exit 2;;
  esac
  SYSROOT=$(xcrun --sdk "$1" --show-sdk-path)
  SDKVER=$(xcrun --sdk "$1" --show-sdk-version)
  L=$BUILD/llvm-$1 CM=$BUILD/cmark-$1 B=$BUILD/swift-$1 O=$BUILD/out/$1
}

checkout() { # dir repo commit-or-tag
  if [[ ! -d $SRC/$1/.git ]]; then
    git init -q "$SRC/$1" && git -C "$SRC/$1" remote add origin "$2"
  fi
  git -C "$SRC/$1" fetch -q --depth 1 origin "$3"
  git -C "$SRC/$1" checkout -q --detach FETCH_HEAD
}

cmd_sources() {
  checkout swift "$SWIFT_REPO" "$SWIFT_COMMIT"
  checkout llvm-project "$LLVM_REPO" "$LLVM_COMMIT"
  checkout cmark "$CMARK_REPO" "refs/tags/$CMARK_TAG"
  checkout swift-experimental-string-processing "$STRING_PROCESSING_REPO" "refs/tags/$STRING_PROCESSING_TAG"
  checkout swift-syntax "$SWIFT_SYNTAX_REPO" "refs/tags/$SWIFT_SYNTAX_TAG"
}

cmd_native() {
  cmake -G Ninja -S "$SRC/llvm-project/llvm" -B "$BUILD/native" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DLLVM_TARGETS_TO_BUILD=WebAssembly -DLLVM_ENABLE_PROJECTS=clang \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
  ninja -C "$BUILD/native" -j "$JOBS" llvm-tblgen clang-tblgen
}

cmd_slice() {
  local s=$1
  slice_vars "$s"

  # 1. LLVM, clang and lld: static libraries only, WebAssembly target only. With
  # CMAKE_SYSTEM_NAME=iOS CMake does not treat ld64 as Apple's linker, so only
  # the archives are built (the executables would not link).
  [[ -f $L/build.ninja ]] || cmake -G Ninja -S "$SRC/llvm-project/llvm" -B "$L" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_SYSTEM_NAME=$SYS -DCMAKE_OSX_SYSROOT="$SYSROOT" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=27.0 \
    -DLLVM_HOST_TRIPLE=$TRIPLE \
    -DLLVM_TARGETS_TO_BUILD=WebAssembly -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_NATIVE_TOOL_DIR="$NATIVE" -DLLVM_TABLEGEN="$NATIVE/llvm-tblgen" \
    -DCLANG_TABLEGEN="$NATIVE/clang-tblgen" \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_SIGNAL_HANDLERS=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF \
    -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF -DLLVM_ENABLE_BINDINGS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_DOCS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_OBJC_REWRITER=OFF -DCMAKE_MACOSX_BUNDLE=OFF \
    -DCLANG_TOOL_C_INDEX_TEST_BUILD=OFF -DCLANG_TOOL_LIBCLANG_BUILD=OFF -DCLANG_BUILD_TOOLS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF -DLLD_BUILD_TOOLS=OFF \
    -DCMAKE_C_FLAGS="-g0" -DCMAKE_CXX_FLAGS="-g0"
  # shellcheck disable=SC2046 # one target per line
  ninja -C "$L" -j "$JOBS" $(cat "$HERE/scripts/llvm-libs.txt")

  # 2. cmark. -fno-common: Apple clang still emits C tentative definitions as
  # common symbols, which the prelink step cannot hide.
  [[ -f $CM/build.ninja ]] || cmake -G Ninja -S "$SRC/cmark" -B "$CM" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_SYSTEM_NAME=$SYS -DCMAKE_OSX_SYSROOT="$SYSROOT" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=27.0 -DCMAKE_MACOSX_BUNDLE=OFF \
    -DBUILD_TESTING=OFF -DCMARK_THREADING=ON "-DCMAKE_C_FLAGS=-g0 -fno-common" -DCMAKE_CXX_FLAGS=-g0
  ninja -C "$CM" -j "$JOBS" libcmark-gfm libcmark-gfm-extensions

  # 3. Swift compiler libraries. Like build-script, Swift keeps
  # CMAKE_SYSTEM_NAME=Darwin and takes the host from SWIFT_HOST_VARIANT_SDK;
  # the iOS SDK comes from the sysroot and -target. SwiftCompilerSources,
  # swift-syntax and ASTGen are compiled with Xcode's swiftc (hosttools).
  # swift-syntax is built as static libraries (macros, linked into the
  # prelinked object); executable plugins are compiled out (no spawning).
  # The plugin server tool stays configured (swift-frontend depends on its
  # in-process library target when swift-syntax is on) but is never built.
  # SourceKit needs SWIFT_ENABLE_DISPATCH (Darwin uses the system libdispatch;
  # nothing is built for it). HAVE_XPC_H is preset OFF so that check_include_files
  # keeps it: sourcekitd then uses its portable in-process API
  # (sourcekitdAPI-InProc.cpp) instead of libxpc, and no XPC service is configured.
  # The install components are the defaults without sourcekit-inproc and
  # sourcekit-xpc-service: installing sourcekit-inproc makes sourcekitdInProc a
  # framework. Only its objects and archives are used, never the linked library.
  local sourcekit=(-DSWIFT_BUILD_SOURCEKIT=OFF -DSWIFT_TOOL_SOURCEKIT_BUILD=OFF -DSWIFT_ENABLE_DISPATCH=OFF)
  if [[ $SOURCEKIT == 1 ]]; then
    sourcekit=(-DSWIFT_BUILD_SOURCEKIT=ON -DSWIFT_TOOL_SOURCEKIT_BUILD=ON -DSWIFT_ENABLE_DISPATCH=ON \
      -DHAVE_XPC_H=OFF -DBUILD_SOURCEKIT_XPC_SERVICE=OFF -DSWIFT_ENABLE_SOURCEKIT_TESTS=OFF \
      -DSOURCEKITD_BUILD_STATIC_INPROC=OFF \
      "-DSWIFT_INSTALL_COMPONENTS=autolink-driver;compiler;compiler-swift-syntax-lib;clang-builtin-headers;libexec;stdlib;sdk-overlay;static-mirror-lib;swift-syntax-lib;editor-integration;tools;testsuite-tools;toolchain-tools;toolchain-dev-tools;license;swift-remote-mirror;swift-remote-mirror-headers")
  fi
  if [[ ! -f $B/build.ninja ]]; then
    local deployment=27.0
    [[ $SYS == Darwin ]] || deployment=
    cmake -G Ninja -S "$SRC/swift" -B "$B" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
      -DCMAKE_OSX_SYSROOT="$SYSROOT" -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_MACOSX_BUNDLE=OFF \
      "-DCMAKE_C_FLAGS=-g0 -target $TRIPLE" "-DCMAKE_CXX_FLAGS=-g0 -target $TRIPLE" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=$deployment \
      -DCMAKE_Swift_COMPILER="$(xcrun -f swiftc)" -DCMAKE_Swift_COMPILER_TARGET=$TRIPLE \
      -DSWIFT_HOST_VARIANT="$s" -DSWIFT_HOST_VARIANT_SDK=$VARIANT_SDK -DSWIFT_HOST_VARIANT_ARCH=arm64 \
      -DSWIFT_HOST_TRIPLE=$TRIPLE -DSWIFT_SDKS=$VARIANT_SDK -DSWIFT_PRIMARY_VARIANT_SDK=$VARIANT_SDK \
      -DSWIFT_PRIMARY_VARIANT_ARCH=arm64 \
      -DSWIFT_DARWIN_DEPLOYMENT_VERSION_IOS=27.0 -DSWIFT_DARWIN_DEPLOYMENT_VERSION_OSX=27.0 \
      -DBOOTSTRAPPING_MODE=HOSTTOOLS -DSWIFT_ENABLE_SWIFT_IN_SWIFT=ON \
      -DSWIFT_BUILD_SWIFT_SYNTAX=ON -DSWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE="$SRC/swift-syntax" \
      -DSWIFT_COMPILER_SWIFT_SYNTAX_STATIC=ON -DSWIFT_DISABLE_EXECUTABLE_PLUGINS=ON \
      -DSWIFT_BUILD_REGEX_PARSER_IN_COMPILER=ON \
      -DSWIFT_PATH_TO_STRING_PROCESSING_SOURCE="$SRC/swift-experimental-string-processing" \
      -DSWIFT_BUILD_STDLIB=OFF -DSWIFT_BUILD_SDK_OVERLAY=OFF -DSWIFT_BUILD_DYNAMIC_STDLIB=OFF \
      -DSWIFT_BUILD_STATIC_STDLIB=OFF -DSWIFT_BUILD_DYNAMIC_SDK_OVERLAY=OFF \
      -DSWIFT_BUILD_STATIC_SDK_OVERLAY=OFF -DSWIFT_BUILD_REMOTE_MIRROR=OFF \
      "${sourcekit[@]}" -DSWIFT_INCLUDE_TESTS=OFF -DSWIFT_INCLUDE_TEST_BINARIES=OFF \
      -DSWIFT_INCLUDE_DOCS=OFF -DSWIFT_BUILD_EXAMPLES=OFF -DSWIFT_BUILD_LIBEXEC=OFF \
      -DSWIFT_BUILD_CLANG_OVERLAYS=OFF -DSWIFT_BUILD_STDLIB_EXTRA_TOOLCHAIN_CONTENT=OFF \
      -DSWIFT_ENABLE_BACKTRACING=OFF -DSWIFT_ENABLE_RUNTIME_MODULE=OFF \
      -DSWIFT_INCLUDE_TOOLS=ON -DSWIFT_INCLUDE_APINOTES=OFF \
      -DSWIFT_TOOL_COMPLETE_TEST_BUILD=OFF -DSWIFT_TOOL_LIBMOCKPLUGIN_BUILD=OFF \
      -DSWIFT_TOOL_LIBSTATICMIRROR_BUILD=OFF -DSWIFT_TOOL_LIBSWIFTSCAN_BUILD=OFF \
      -DSWIFT_TOOL_LLDB_MODULEIMPORT_TEST_BUILD=OFF -DSWIFT_TOOL_SOURCEKITD_REPL_BUILD=OFF \
      -DSWIFT_TOOL_SOURCEKITD_TEST_BUILD=OFF \
      -DSWIFT_TOOL_SWIFT_COMPATIBILITY_SYMBOLS_BUILD=OFF -DSWIFT_TOOL_SWIFT_DEF_TO_STRINGS_CONVERTER_BUILD=OFF \
      -DSWIFT_TOOL_SWIFT_DEMANGLE_BUILD=OFF -DSWIFT_TOOL_SWIFT_DEMANGLE_YAMLDUMP_BUILD=OFF \
      -DSWIFT_TOOL_SWIFT_FUNCTION_CALLER_GENERATOR_BUILD=OFF -DSWIFT_TOOL_SWIFT_IDE_TEST_BUILD=OFF \
      -DSWIFT_TOOL_SWIFT_PLUGIN_SERVER_BUILD=ON -DSWIFT_TOOL_SWIFT_REFACTOR_BUILD=OFF \
      -DSWIFT_TOOL_SWIFT_REFLECTION_DUMP_BUILD=OFF -DSWIFT_TOOL_SWIFT_REMOTEAST_TEST_BUILD=OFF \
      -DSWIFT_TOOL_SWIFT_SCAN_TEST_BUILD=OFF -DSWIFT_TOOL_SWIFT_SERIALIZE_DIAGNOSTICS_BUILD=OFF \
      -DSWIFT_NATIVE_LLVM_TOOLS_PATH="$NATIVE" -DSWIFT_NATIVE_CLANG_TOOLS_PATH="$NATIVE" \
      -DLLVM_TABLEGEN="$NATIVE/llvm-tblgen" \
      -DLLVM_DIR="$L/lib/cmake/llvm" -DClang_DIR="$L/lib/cmake/clang" \
      -DSWIFT_PATH_TO_CMARK_SOURCE="$SRC/cmark" -DSWIFT_PATH_TO_CMARK_BUILD="$CM" \
      -DSWIFT_TOOLCHAIN_VERSION="$VERSION" -DSWIFT_APPEND_VC_REV=OFF \
      -DLLVM_ENABLE_ASSERTIONS=OFF
  else
    # An existing tree: apply the version label of this run (it is compiled into swifthost_version()).
    # (-S too: with -B alone CMake takes the current directory as the source.)
    cmake -S "$SRC/swift" -B "$B" -DSWIFT_TOOLCHAIN_VERSION="$VERSION" "${sourcekit[@]}" >/dev/null
  fi
  # Only the archives on swift-frontend's link line: the executable itself
  # needs host-run tools that a cross build cannot run.
  local libs=() lib
  while read -r lib; do libs+=("$lib"); done < <(
    awk '/^build bin\/swift-frontend:/{p=1} p{print} p&&!/\$$/{exit}' "$B/build.ninja" |
    tr ' ' '\n' | grep -E '^lib/.*\.a$' | sort -u)
  # With SourceKit also the objects and archives on the link line of the
  # in-process sourcekitd library (the library itself is not linked).
  if [[ $SOURCEKIT == 1 ]]; then
    while read -r lib; do libs+=("$lib"); done < <(
      awk '/^build lib\/libsourcekitdInProc\.dylib:/{p=1} p{print} p&&!/\$$/{exit}' "$B/build.ninja" |
      tr ' ' '\n' | grep -E '^(lib/.*\.a|tools/.*\.o)$' | sort -u)
  fi
  ninja -C "$B" -j "$JOBS" "${libs[@]}"

  # 4. The shim, prelinked with every archive it needs into one object that
  # exports only the swifthost_* symbols (with SourceKit: and sourcekitd's
  # objects and archives, exporting its C API as well).
  mkdir -p "$O"
  # With SourceKit the shim also holds sourcekitd's crash containment (SourceKit's work-queue and service hooks).
  local shimSourceKit=()
  if [[ $SOURCEKIT == 1 ]]; then
    shimSourceKit=(-DSWIFTHOST_SOURCEKIT=1 -I"$SRC/swift/tools/SourceKit/include"
      -I"$SRC/swift/tools/SourceKit/tools/sourcekitd/include" -I"$B/tools/SourceKit/include")
  fi
  "$CXX" -target $TRIPLE -isysroot "$SYSROOT" -std=c++17 -O2 -g0 -fPIC \
    -fno-exceptions -fno-rtti -funwind-tables -fsized-deallocation -fvisibility-inlines-hidden \
    -DNDEBUG -DCMARK_STATIC_DEFINE -DGTEST_HAS_RTTI=0 -DOBJC_OLD_DISPATCH_PROTOTYPES=0 \
    -DSWIFT_ENABLE_SWIFT_IN_SWIFT -DSWIFT_LLVM_SUPPORT_IS_AVAILABLE -DSWIFT_THREADING_DARWIN \
    -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS \
    -I"$HERE/include" -I"$SRC/swift/stdlib/public/SwiftShims" -I"$B/include" -I"$SRC/swift/include" \
    -I"$SRC/llvm-project/llvm/include" -I"$L/include" \
    -I"$SRC/llvm-project/clang/include" -I"$L/tools/clang/include" \
    -I"$SRC/llvm-project/lld/include" -I"$L/tools/lld/include" \
    ${shimSourceKit[@]+"${shimSourceKit[@]}"} -Wall -Wno-unused-parameter \
    -c "$HERE/src/SwiftHostCompiler.cpp" -o "$O/SwiftHostCompiler-shim.o"
  local archives=() objects=() a
  while read -r a; do
    case $a in /*) ;; *) a=$B/$a;; esac
    case $a in *.o) objects+=("$a");; *) archives+=("$a");; esac
  done < <(cd "$B" && {
    ninja -t commands bin/swift-frontend | tail -1 | tr ' ' '\n' | grep -E '\.a$'
    [[ $SOURCEKIT != 1 ]] ||
      ninja -t commands lib/libsourcekitdInProc.dylib | tail -1 | tr ' ' '\n' | grep -E '\.(a|o)$'
  } | awk '!s[$0]++')
  printf '%s\n' _swifthost_version _swifthost_swift_frontend _swifthost_swift_autolink_extract _swifthost_wasm_ld \
    _swifthost_request_cancel _swifthost_set_plugin_message_handler _swifthost_sourcekitd_status > "$O/exports.txt"
  # sourcekitd's C API (the portable list; the -darwin one adds C++ symbols
  # for the XPC service). -u pulls in the archive members that define them.
  local undefined=()
  if [[ $SOURCEKIT == 1 ]]; then
    sed 's/^/_/' "$SRC/swift/tools/SourceKit/tools/sourcekitd/bin/InProc/sourcekitdInProc.exports" >> "$O/exports.txt"
    while read -r a; do undefined+=(-u "$a"); done < "$O/exports.txt"
  fi
  archives+=("$L/lib/liblldWasm.a" "$L/lib/liblldCommon.a")
  # Swift libraries built by CMake (swift-syntax, ASTGen, the regex parser)
  # carry autolink entries for themselves (-l_CompilerSwiftSyntax, ...). They
  # are linked into the object, so drop those entries; the app's link would
  # warn about each library it cannot find.
  local ignored=()
  for a in "${archives[@]}"; do
    a=${a##*/} a=${a#lib}
    ignored+=(-ignore_auto_link_library "${a%.a}")
  done
  xcrun ld -r -arch arm64 -platform_version $PLAT 27.0 "$SDKVER" -exported_symbols_list "$O/exports.txt" \
    "${ignored[@]}" ${undefined[@]+"${undefined[@]}"} -o "$O/SwiftHostCompiler.o" "$O/SwiftHostCompiler-shim.o" \
    ${objects[@]+"${objects[@]}"} "${archives[@]}"
  local exported
  exported=$(xcrun nm -gU "$O/SwiftHostCompiler.o" | awk '{print $3}' | sort | tr '\n' ' ')
  if [[ $exported != "$(sort "$O/exports.txt" | tr '\n' ' ')" ]]; then
    echo "unexpected exported symbols: $exported" >&2
    exit 1
  fi
  ls -l "$O/SwiftHostCompiler.o"
}

cmd_package() {
  local id=$1 D=$BUILD/dist/$1 args=()
  rm -rf "$D" && mkdir -p "$D/headers"
  cp "$HERE/include/SwiftHostCompiler.h" "$HERE/include/module.modulemap" "$D/headers/"
  : > "$D/sizes.txt"
  for s in iphoneos iphonesimulator macosx; do
    # The release id must be the version label compiled into this slice (VERSION when it was built).
    if ! strings -a "$BUILD/out/$s/SwiftHostCompiler.o" | grep -x -F -- "$id" >/dev/null; then
      echo "$s: SwiftHostCompiler.o does not contain the version label $id; rebuild it with VERSION=$id" >&2
      exit 1
    fi
    mkdir -p "$D/libs/$s"
    xcrun libtool -static -o "$D/libs/$s/libSwiftHostCompiler.a" "$BUILD/out/$s/SwiftHostCompiler.o"
    args+=(-library "$D/libs/$s/libSwiftHostCompiler.a" -headers "$D/headers")
    printf '%s libSwiftHostCompiler.a: %s bytes\n' "$s" "$(stat -f %z "$D/libs/$s/libSwiftHostCompiler.a")" >> "$D/sizes.txt"
  done
  xcodebuild -create-xcframework "${args[@]}" -output "$D/SwiftHostCompiler.xcframework" >/dev/null
  cp "$HERE/JOBS.txt" "$HERE/README.md" "$D/SwiftHostCompiler.xcframework/"
  # License files of everything linked in, byte for byte from the sources.
  mkdir -p "$D/licenses"
  cp "$SRC/llvm-project/llvm/LICENSE.TXT" "$D/licenses/llvm-LICENSE.TXT"
  cp "$SRC/llvm-project/clang/LICENSE.TXT" "$D/licenses/clang-LICENSE.TXT"
  cp "$SRC/llvm-project/lld/LICENSE.TXT" "$D/licenses/lld-LICENSE.TXT"
  cp "$SRC/llvm-project/llvm/lib/Support/BLAKE3/LICENSE" "$D/licenses/llvm-Support-BLAKE3-LICENSE"
  cp "$SRC/llvm-project/llvm/lib/Support/COPYRIGHT.regex" "$D/licenses/llvm-Support-COPYRIGHT.regex"
  cp "$SRC/swift/LICENSE.txt" "$D/licenses/swift-LICENSE.txt"
  cp "$SRC/cmark/COPYING" "$D/licenses/swift-cmark-COPYING"
  cp "$SRC/swift-experimental-string-processing/LICENSE.txt" "$D/licenses/swift-experimental-string-processing-LICENSE.txt"
  cp "$SRC/swift-syntax/LICENSE.txt" "$D/licenses/swift-syntax-LICENSE.txt"
  cp -R "$D/licenses" "$D/SwiftHostCompiler.xcframework/licenses"
  (cd "$D" && ditto -c -k --norsrc --noextattr --keepParent SwiftHostCompiler.xcframework "SwiftHostCompiler-$id.xcframework.zip")
  cp "$HERE/JOBS.txt" "$D/JOBS.txt"
  # One line per release asset; the license files are attached by their file names.
  (cd "$D" && shasum -a 256 "SwiftHostCompiler-$id.xcframework.zip" JOBS.txt > SHA256SUMS)
  (cd "$D/licenses" && shasum -a 256 -- * >> ../SHA256SUMS)
  ls -l "$D"
}

case ${1:-} in
  sources) cmd_sources;;
  native)  cmd_native;;
  slice)   cmd_slice "${2:?slice}";;
  package) cmd_package "${2:?release id}";;
  *) sed -n '2,23p' "$0"; exit 2;;
esac
