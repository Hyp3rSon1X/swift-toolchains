# Swift 6.4.2 SDK for WebAssembly with threads and tail calls

This is the recipe for the `swift-6.4.2-RELEASE_wasm-threads-tailcall` Swift SDK artifact bundle. The target is
`wasm32-unknown-wasip1-threads`, like the plain threads SDK built by the parent directory, but async code uses real tail
calls.

## Why

Swift 6.4.2 has no `swifttailcc` for WebAssembly. Async functions use `swiftcc`, so every continuation is an ordinary
call. Each `await` that does not suspend nests one frame on the WebAssembly stack, for example an inline actor hop, an
executor switch, or an async call that returns at once. A loop of about 1,700 such awaits overflows a 1 MiB stack, and
long hop chains corrupt memory before that.

With the WebAssembly tail-call proposal enabled (`-mtail-call`), the pieces fit together:
- clang accepts `swiftasynccall` for the runtime;
- Swift IRGen emits `swifttailcc` with `musttail`;
- the backend lowers every continuation to `return_call`.

The stack depth then stays flat: 10,000,000 awaits and 1,000,000 actor hops run in a 64 KiB stack.

## Sources

Same as the parent recipe (`../fetch-src.sh`, tag `swift-6.4.2-RELEASE`), except for two repositories:

- `swift`: Hyp3rSon1X/swift, branch `hyp3r/wasm-sdk/6.4.2`, `37591f512993731dc2a9739dfb2912fdf5d17775`.
  It is `swift-6.4.2-RELEASE` (`83eb12448e4`) plus three cherry-picks and one fix of our own (the SDK `-2` was built from
  `a9c0b4f09a79885037a86d377ccb470efee6a939`, which lacks the last one):
  - `d7dcf221748b7fb3d9fbedb45fd6df20f5cc8748`, swiftlang/swift#88074, "Wasm: add tests for swiftasyncc tail calls".
    Tests only; the next pick's test edits need it.
  - `3c9966afca9c1e207dcf0b23b39971d81c3bdff6`, swiftlang/swift#93062, "[IRGen] Preserve async-context attributes in
    concurrency runtime metadata". It adds `swiftasync` to the declarations of `swift_task_switch` and
    `swift_asyncLet_finish`. Without it, the backend gives the declaration and the runtime definition different wasm
    signatures, and every actor hop or `async let` produces an invalid module.
  - `1f3eac39b6aa7401100afbd931eec3a68acf8d7a`, swiftlang/swift#93586, "[Concurrency] Don't sleep while the
    cooperative executor has jobs to run".
  - `37591f512993731dc2a9739dfb2912fdf5d17775`, "[Synchronization] Take the wasm Mutex as contended after waiting"
    (new in `-3`, not yet upstream). See "Changes in -3".
- `llvm-project`: Hyp3rSon1X/llvm-project, branch `hyp3r/wasm-sdk/6.4.2`,
  `11627bca6d44ea33b0fbb74d4c5c0e6c45acc087`. It is `swift-6.4.2-RELEASE` (`fb5f84d903a3`) plus three cherry-picks:
  - `b9c55a400656eb43be1215828cee8f6eaa970e94`, swiftlang/llvm-project#12648 (a squash of llvm/llvm-project#188296
    and #203330), "Wasm: support tail calls in `CallingConv::SwiftTail`".
  - `fd9bc2e181b7bb10baffb64662c924bbf52f3734`, llvm/llvm-project#230073, "[WebAssembly] Preserve tail-call pseudos
    during stack-protector splitting". Without it, a tail call in a function with a stack protector is invalid.
  - `38e6c0073c423c3945a92d8bb4bf43db029b9d56`, llvm/llvm-project#227491, "[WebAssembly] Do not tail call with byval
    arguments". Without it, a struct passed by value in tail position points into a released frame.

`build-system.patch` touches `utils/swift_build_support` only. It adds `-mtail-call` (C/C++) and `-Xcc -mtail-call`
(Swift) to the threads stdlib and runtime, and to the threads builds of libxml2, Foundation (swift-foundation,
swift-corelibs-foundation, FoundationICU), swift-testing and XCTest. The plain `wasm32-unknown-wasip1` target keeps
upstream's flags.

## Changes in -3

`swifthost-sdk-wasip1-threads-6.4.2-3` fixes a lost wakeup in `Synchronization.Mutex` on `wasm32-unknown-wasip1-threads`
(`stdlib/public/Synchronization/Mutex/WasmImpl.swift`). A waiter that had been woken took the lock with a plain
`compareExchange(0, 1)`, so the lock word said "locked, nobody waiting". When another waiter was still asleep, the
`unlock` of the new owner then skipped `memory.atomic.notify`, and that sleeper was never woken. The program hangs at 0 %
CPU. With four pthreads and 100,000 `withLock` calls each, the `-2` SDK hung in 18 of 20 runs. The fix is the
Linux/Drepper rule: after every wake, take the lock as contended (`exchange(.contended)`), so the next `unlock` always
wakes a waiter.

`-3` differs from `-2` in `libswiftSynchronization.a` only; the Synchronization module and interface files, the
compiler, llvm-project sources, build-system patch, toolset and every other file of the bundle are byte-identical. `_MutexHandle._lock()` is not
inlinable, so code compiled against `-2` is fixed by relinking against the `-3` archive.

The regression test is `test/stdlib/Synchronization/Mutex/WasmMutexHandoff.swift` in the swift fork commit (an
executable test for `OS=wasip1`, copied here as `test/WasmMutexHandoff.swift`). `test/mutex_stress.sh` is the SDK-level check.

## Steps (macOS arm64, Xcode 27.2, cmake and ninja on PATH)

```
export W=<empty build directory>
../fetch-src.sh                                 # as for the plain threads SDK
./use-forks.sh                                  # swift and llvm-project to the fork commits, applies build-system.patch
../build.sh                                     # upstream build-script --build-wasi-stdlib (ends with "build-script exit 1": see below)
../gen-sdk.sh wasm32-unknown-wasip1-threads
./package.sh                                    # writes $W/dist-tailcall/swift-6.4.2-RELEASE_wasm-threads-tailcall.artifactbundle.zip
```

`../build.sh` ends with `build-script exit 1` at the swiftsyntax `swift run` step. That is expected once all WebAssembly
products are built (the just-built toolchain has no `swift-run`).

If `$W` already holds a build of other sources: the standalone stdlib builds do not depend on the compiler binary.
Delete `$W/build/wasm/wasithreadsstdlib-macosx-arm64` and `$W/build/wasm/wasiswiftsdk-macosx-arm64/*/wasm32-unknown-wasip1-threads`
before `../build.sh`, or nothing is recompiled with the new compiler.

Differences from the parent recipe:
- The swift and llvm-project sources and `build-system.patch` described above.
- `package.sh` renames the bundle, artifact and info.json entry to `swift-6.4.2-RELEASE_wasm-threads-tailcall`. The
  triple stays the same, so select the SDK by its id.
- The toolset adds `-Xcc -mtail-call` to `swiftCompiler`, and `-mtail-call` to `cCompiler` and `cxxCompiler`.
- The Embedded variant is removed, as in the parent recipe.

## Using the SDK

- Compile Swift with `-Xcc -mtail-call` and C/C++ with `-mtail-call`. The toolset does this for SwiftPM.
- Code built without the flag is not supported. It links, but calls to `swift_continuation_await`
  (`withCheckedContinuation`, `withUnsafeContinuation`) trap. The caller's own frames also still nest.
- The Swift compiler needs the swift pick #93062, and its LLVM needs #12648, #230073 and #227491. SwiftHostCompiler
  `swifthost-6.4.2-3` has all four. The swift.org 6.4.2 toolchain does not: with it, `-Xcc -mtail-call`
  has no effect on async code, which is the unsupported case above.
- The runtime needs the tail-call proposal. For WAMR, use `-DWAMR_BUILD_TAIL_CALL=1` and
  `-DWAMR_BUILD_LIB_WASI_THREADS=1`.

## Tests (`test/`)

The probes:
- `pa_await_loop.swift`: N awaits on an async function that never suspends (default 1,000,000);
- `pb_actor_hops.swift`: N calls from a detached task to an idle actor and back (default 100,000);
- `pe_patterns.swift`: 100,000 awaits each through these shapes: protocol witness, closure, throwing, generic,
  checked and unsafe continuation, `async let`, TaskGroup child, and `MainActor.run`;
- `pf_coop_timer.swift`: timing of a 20 ms sleep and of `Task.yield()` while a 1 s sleep is pending (swift#93586);
- `mutex_stress.sh <label> <swiftc> <bundle-dir> <bundle-id> <iwasm> [p3-runs ab-runs handoff-runs]` compiles three programs
  against the bundle and runs each many times with a timeout, counting hangs (see "Changes in -3"): `../../test/p3_mutex.swift`
  (four pthreads, 1,000 `withLock` each), `mutex_ab.swift std` (four pthreads, 100,000 each) and `WasmMutexHandoff.swift`
  (the swift repository's regression test);
- `byval.c`: structs passed by value in tail position, which must not become `return_call`, plus a control call that
  must;
- `common.swift`: progress output for the probes, written to stderr.

Scripts:
- `run-probes.sh <label> <swiftc> <bundle-dir> <bundle-id> <tc|notc> <iwasm>` compiles pa, pb, pe and pf. It checks
  the link for signature-mismatch warnings, validates with node when node is installed, and runs each probe at a
  1 MiB and a 64 KiB stack.
- `run-swiftc.sh` and `run-swifthost.sh` build the parent's p1-p4 probes with a `swiftc` or with SwiftHostCompiler,
  with or without `-Xcc -mtail-call`. Each probe is linked with both link variants. `run-iwasm.sh <dir> <iwasm>` runs
  the results.
- `byval.sh <clang> <threads-sdk-dir> <out-dir> <iwasm>` compiles `byval.c` with `-mtail-call`, prints the
  `call`/`return_call` use of each function, and runs it.
- `validate.mjs <file.wasm>` compiles a module with V8. Its error names the function and the instruction, which is more
  precise than WAMR's loader message.

The probes need an `iwasm` (WAMR 2.4.5) built with `-DWAMR_BUILD_LIB_WASI_THREADS=1 -DWAMR_BUILD_TAIL_CALL=1
-DWAMR_BUILD_FAST_INTERP=1`.
