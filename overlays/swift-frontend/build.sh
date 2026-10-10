#!/bin/bash
# Compiler overlay: rebuilds swift-frontend (the fix lives in lib/LLVMPasses, linked statically into it) from the
# patched swift tree plus the unmodified swift-6.4.0-RELEASE LLVM/Clang, and keeps only that binary.
# Runs inside the official Swift image of the matching version (host compiler, libstdc++ and glibc then equal
# the official toolchain's build environment).
#
# Usage: build.sh llvm  <workspace> <build-dir> <out-dir>   cmark + LLVM/Clang libraries swift-frontend links
#        build.sh swift <workspace> <build-dir> <out-dir>   swift-frontend (needs the llvm stage's build-dir)
#   <workspace> holds checkouts named swift, llvm-project, cmark, swift-syntax, swift-experimental-string-processing.
#
# Flags mirror what utils/build-script passes for `--release --no-assertions` with the default LLVM target list
# (checked with `build-script --dry-run`); only products that do not end up in swift-frontend are left out:
# clang-tools-extra, lld, compiler-rt, tests/docs/examples, and the stdlib (configured, never built).
set -euo pipefail
STAGE="$1"; WS="$2"; B="$3"; OUT="$4"
JOBS="${JOBS:-$(nproc)}"
ARCH="$(uname -m)"
CFLAGS_COMMON="-Wno-unknown-warning-option -Werror=unguarded-availability-new -fno-stack-protector"
LLVM_B="$B/llvm"; CMARK_B="$B/cmark"; SWIFT_B="$B/swift"

install_deps() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq cmake ninja-build python3 libicu-dev libcurl4-openssl-dev libedit-dev libsqlite3-dev \
    libxml2-dev pkg-config rsync uuid-dev zlib1g-dev libncurses-dev libpython3-dev ccache zstd >/dev/null
}

# Libraries swift-frontend links, in ninja target spelling (static archives lib<name>.a in the LLVM build tree).
frontend_llvm_targets() {
  ninja -C "$SWIFT_B" -t commands bin/swift-frontend | tail -1 | tr ' ' '\n' \
    | grep -F "$LLVM_B" | grep -v '^-L' | grep '\.a$' | sed 's#.*/lib/lib##; s#\.a$##' | sort -u
}

configure_cmark() {
  mkdir -p "$CMARK_B"
  cmake -S "$WS/cmark" -B "$CMARK_B" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMARK_THREADING=ON "-DCMAKE_C_FLAGS=$CFLAGS_COMMON" "-DCMAKE_CXX_FLAGS=$CFLAGS_COMMON" >/dev/null
}

configure_llvm() {
  mkdir -p "$LLVM_B"
  cmake -S "$WS/llvm-project/llvm" -B "$LLVM_B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ${CCACHE:+-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache} \
    -DLLVM_ENABLE_ASSERTIONS=FALSE '-DLLVM_TARGETS_TO_BUILD=X86;ARM;AArch64;PowerPC;SystemZ;Mips;RISCV;WebAssembly;AVR;BPF' \
    -DCMAKE_INSTALL_PREFIX=/usr "-DCMAKE_C_FLAGS=$CFLAGS_COMMON" "-DCMAKE_CXX_FLAGS=$CFLAGS_COMMON" \
    '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -DNDEBUG' '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -DNDEBUG' \
    -DLLVM_TOOL_SWIFT_BUILD=FALSE -DLLVM_INCLUDE_DOCS=FALSE -DLLVM_INCLUDE_TESTS=FALSE -DLLVM_INCLUDE_BENCHMARKS=FALSE \
    -DLLVM_INCLUDE_EXAMPLES=FALSE -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF -DLLVM_ENABLE_PROJECTS=clang >/dev/null
}

configure_swift() {
  mkdir -p "$SWIFT_B"
  cmake -S "$WS/swift" -B "$SWIFT_B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_Swift_COMPILER="$(command -v swiftc)" \
    ${CCACHE:+-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache} \
    "-DCMAKE_C_FLAGS=$CFLAGS_COMMON" "-DCMAKE_CXX_FLAGS=$CFLAGS_COMMON" \
    -DLLVM_ENABLE_ASSERTIONS=FALSE -DSWIFT_STDLIB_ASSERTIONS=FALSE \
    -DSWIFT_ENABLE_EXPERIMENTAL_DIFFERENTIABLE_PROGRAMMING=TRUE -DSWIFT_ENABLE_EXPERIMENTAL_CONCURRENCY=TRUE \
    -DSWIFT_ENABLE_EXPERIMENTAL_CXX_INTEROP=TRUE -DSWIFT_ENABLE_CXX_INTEROP_SWIFT_BRIDGING_HEADER=TRUE \
    -DSWIFT_ENABLE_EXPERIMENTAL_DISTRIBUTED=TRUE -DSWIFT_ENABLE_BACKTRACING=TRUE -DSWIFT_ENABLE_EXPERIMENTAL_OBSERVATION=TRUE \
    -DSWIFT_ENABLE_SYNCHRONIZATION=TRUE -DSWIFT_ENABLE_VOLATILE=TRUE -DSWIFT_ENABLE_RUNTIME_MODULE=TRUE \
    -DSWIFT_ENABLE_EXPERIMENTAL_PARSER_VALIDATION=TRUE -DSWIFT_ENABLE_EXPERIMENTAL_STRING_PROCESSING=TRUE \
    -DSWIFT_INCLUDE_TOOLS=TRUE -DSWIFT_INCLUDE_TESTS=FALSE -DSWIFT_INCLUDE_DOCS=FALSE -DSWIFT_BUILD_EXAMPLES=FALSE \
    -DSWIFT_BUILD_PERF_TESTSUITE=FALSE -DSWIFT_ENABLE_DISPATCH=FALSE -DSWIFT_SOURCEKIT_USE_INPROC_LIBRARY=TRUE \
    -DSWIFT_AST_VERIFIER=TRUE -DSWIFT_SIL_VERIFY_ALL=FALSE -DSWIFT_SDKS=LINUX -DCMAKE_INSTALL_PREFIX=/usr/ \
    -DClang_DIR="$LLVM_B/lib/cmake/clang" -DLLVM_DIR="$LLVM_B/lib/cmake/llvm" \
    -DCLANG_VERSION_MAJOR=21 -DCLANG_VERSION_MINOR=0 -DCLANG_VERSION_PATCH=0 \
    -DLLVM_VERSION_MAJOR=21 -DLLVM_VERSION_MINOR=0 -DLLVM_VERSION_PATCH=0 \
    -DSWIFT_PATH_TO_CMARK_SOURCE="$WS/cmark" -DSWIFT_PATH_TO_CMARK_BUILD="$CMARK_B" \
    -DSWIFT_PATH_TO_STRING_PROCESSING_SOURCE="$WS/swift-experimental-string-processing" \
    -DSWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE="$WS/swift-syntax" -DSWIFT_BUILD_SWIFT_SYNTAX=TRUE \
    -DSWIFT_HOST_VARIANT=linux -DSWIFT_HOST_VARIANT_SDK=LINUX -DSWIFT_HOST_VARIANT_ARCH="$ARCH" >/dev/null
}

case "$STAGE" in
  llvm)
    install_deps
    configure_cmark; ninja -C "$CMARK_B" -j"$JOBS"
    configure_llvm
    # The swift configure step needs llvm-tblgen; the link line then names the exact libraries to build.
    ninja -C "$LLVM_B" -j"$JOBS" llvm-tblgen clang-tblgen
    configure_swift
    mapfile -t targets < <(frontend_llvm_targets)
    echo "llvm stage: ${#targets[@]} libraries"
    ninja -C "$LLVM_B" -j"$JOBS" "${targets[@]}" clang-resource-headers
    mkdir -p "$OUT"
    ;;
  swift)
    install_deps
    [ -f "$LLVM_B/lib/cmake/llvm/LLVMConfig.cmake" ] || { echo "::error::llvm stage output missing in $LLVM_B"; exit 1; }
    configure_cmark; ninja -C "$CMARK_B" -j"$JOBS"
    configure_swift
    ninja -C "$SWIFT_B" -j"$JOBS" swift-frontend
    install -D -m 0755 "$SWIFT_B/bin/swift-frontend" "$OUT/usr/bin/swift-frontend"
    stock=/usr/bin/swift-frontend
    {
      echo "component: swift-frontend (compiler overlay, SC-0004)"
      echo "swift: $(git -c safe.directory='*' -C "$WS/swift" rev-parse HEAD)"
      for d in llvm-project cmark swift-syntax swift-experimental-string-processing; do
        echo "$d: $(git -c safe.directory='*' -C "$WS/$d" rev-parse HEAD) ($(git -c safe.directory='*' -C "$WS/$d" describe --tags --match 'swift-*-RELEASE' --exact-match 2>/dev/null || echo untagged))"
      done
      echo "arch: $ARCH"
      echo "host compiler: $(clang --version | head -1); $(swiftc --version 2>&1 | head -1)"
      echo "version stock:   $($stock -version 2>&1 | head -1)"
      echo "version patched: $(LD_LIBRARY_PATH=/usr/lib/swift/linux:/usr/lib/swift/host/compiler "$OUT/usr/bin/swift-frontend" -version 2>&1 | head -1)"
      echo "size stock/patched: $(stat -c %s $stock) / $(stat -c %s "$OUT/usr/bin/swift-frontend")"
      echo "NEEDED stock==patched: $(diff <(readelf -d $stock | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort) <(readelf -d "$OUT/usr/bin/swift-frontend" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort) >/dev/null && echo yes || echo NO)"
      (cd "$OUT" && sha256sum usr/bin/swift-frontend)
    } > "$OUT/MANIFEST-swift-frontend.txt"
    cat "$OUT/MANIFEST-swift-frontend.txt"
    ;;
  *) echo "unknown stage $STAGE" >&2; exit 2 ;;
esac
