# Swift 6.4.2 SDK for WebAssembly with threads

Recipe for the `swifthost-sdk-wasip1-threads-6.4.2-1` release: a Swift SDK artifact bundle
(`swift-6.4.2-RELEASE_wasm-threads`) for the target `wasm32-unknown-wasip1-threads`, built from the unmodified
`swift-6.4.2-RELEASE` sources.

[`tailcall/`](tailcall/README.md) builds `swifthost-sdk-wasip1-threads-6.4.2-2` and `-3`
(`swift-6.4.2-RELEASE_wasm-threads-tailcall`): the same target with WebAssembly tail calls for async code, from swift
and llvm-project sources with upstream fixes picked.

Upstream 6.4.2 already builds this target. `wasiswiftsdk.py` builds the threads stdlib, Foundation, swift-testing and
XCTest, but packages only `wasm32-unknown-wasip1` (`build_sdk=False` for threads). This recipe runs upstream's build and
then packages the threads target with the same swift-sdk-generator command.

## Steps (macOS arm64, Xcode 27.2, cmake and ninja on PATH)

```
export W=<empty build directory>
./fetch-src.sh          # release/6.4.2 scheme refs; LOCAL_CHECKOUT=<update-checkout tree> makes worktrees instead of clones
./build.sh              # upstream build-script --build-wasi-stdlib (about 1 h at 4 jobs on an M-class Mac)
./gen-sdk.sh wasm32-unknown-wasip1-threads   # swift-sdk-generator 6.4.2 via `xcrun swift run`
./package.sh            # drops the Embedded variant (none is built for this triple), deterministic zip
```

Differences from upstream's own build:
- swiftpm is not built, so the generator runs with the host `swift`. Its arguments are the same as in `wasiswiftsdk.py`.
- LLVM targets are AArch64 and WebAssembly only.
- No tests are run, and wasmkit is not built: upstream uses it only to run the wasm stdlib tests.
- The threads bundle has no Embedded Swift variant.

`SWIFT_TOOLCHAIN_VERSION=swift-6.4.2-RELEASE` is set, so the modules record "Swift version 6.4.2
(swift-6.4.2-RELEASE)". `SWIFT_COMPILER_VERSION` stays empty, as in swift.org builds. The serialization revision check
applies only to compilers that set it.

## Tests (`test/`)

The probes are:
- `p1`: a TaskGroup of 100 tasks;
- `p2`: four pthreads adding to a Synchronization `Atomic<Int>`;
- `p3`: a `Mutex` shared by four pthreads;
- `p4`: Foundation (`Date`, `JSONEncoder`).

Scripts:
- `run-swiftorg.sh <label> <bundle-dir>` compiles and links the probes with the swift.org 6.4.2 `swiftc`. It expects
  the pkg expanded at `$W/tools/swiftorg-6.4.2`.
- `run-swifthost.sh <label> <bundle-dir>` does the same through SwiftHostCompiler `swifthost-6.4.2-2`, using
  `swifthost_drv.c` built against the macos-arm64 slice at `$W/tools/swifthost_drv`.
- `run-iwasm.sh <dir>` runs the results in WAMR 2.4.5 `iwasm` built with `-DWAMR_BUILD_LIB_WASI_THREADS=1`.

Each probe is linked twice: with the toolset's linker flags (imported shared memory), and with
`--shared-memory --export-memory --max-memory=N` without `--import-memory`. `wasminfo.py` shows where the memory comes
from and the start function.
