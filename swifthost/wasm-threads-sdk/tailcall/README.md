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

Same as the parent recipe (`../fetch-src.sh`, tag `swift-6.4.2-RELEASE`), except for three repositories:

- `swift`: Hyp3rSon1X/swift, branch `hyp3r/wasm-sdk/6.4.2`, `6c40b6bc98c804e83d339a9db7e1538256dd5fbc`.
  It is `swift-6.4.2-RELEASE` (`83eb12448e4`) plus four cherry-picks and three fixes of our own (the SDK `-4` was built from
  `37591f512993731dc2a9739dfb2912fdf5d17775`, which lacks the last three, and `-5` from
  `b906bfb4bb352e9c935b6750d23c533da9d6de70`, which lacks the last one):
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
  - `44bbd0de55f` (cherry-picked from `a410437bcfac6a87609f761ac8d7c3520f6085a3`), swiftlang/swift#94088, "[Runtime] Fix
    _swift_stdlib_immortalize producing objects that revert to regular refcounting" (new in `-5`). See "Changes in -5".
  - `b906bfb4bb352e9c935b6750d23c533da9d6de70`, "[Runtime] Define _swift_stdlib_immortalize with the C calling
    convention" (new in `-5`, not yet upstream). See "Changes in -5".
  - `6c40b6bc98c804e83d339a9db7e1538256dd5fbc`, "[Serialization] Serialize differentiability witnesses in a deterministic order" (new in `-6`,
    not yet upstream). See "Changes in -6".
- `llvm-project`: Hyp3rSon1X/llvm-project, branch `hyp3r/wasm-sdk/6.4.2`,
  `11627bca6d44ea33b0fbb74d4c5c0e6c45acc087`. It is `swift-6.4.2-RELEASE` (`fb5f84d903a3`) plus three cherry-picks:
  - `b9c55a400656eb43be1215828cee8f6eaa970e94`, swiftlang/llvm-project#12648 (a squash of llvm/llvm-project#188296
    and #203330), "Wasm: support tail calls in `CallingConv::SwiftTail`".
  - `fd9bc2e181b7bb10baffb64662c924bbf52f3734`, llvm/llvm-project#230073, "[WebAssembly] Preserve tail-call pseudos
    during stack-protector splitting". Without it, a tail call in a function with a stack protector is invalid.
  - `38e6c0073c423c3945a92d8bb4bf43db029b9d56`, llvm/llvm-project#227491, "[WebAssembly] Do not tail call with byval
    arguments". Without it, a struct passed by value in tail position points into a released frame.
- `wasi-libc`: Hyp3rSon1X/wasi-libc, branch `hyp3r/wasm-sdk/6.4.2`, `85613c6915f511541354e6f285e6d6a59900581d`
  (new in `-4`). It is Swift 6.4.2's wasi-libc pin (`2fc32bc81b9f`) plus one commit of our own, "Export a per-thread
  __stack_limit global on wasi-threads" (not yet upstream). See "Changes in -4".

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

## Changes in -4

`swifthost-sdk-wasip1-threads-6.4.2-4` lets a runtime detect a stack overflow on a pthread. wasi-libc gives a pthread a
stack from `malloc()` (by default min(main stack, 8 MiB)) with no guard below it, because there is no `mprotect`. Only
libc knows where that stack ends, so an overflow runs into the heap blocks below the stack and corrupts them silently
before anything traps.

The contract:
- A module that links `pthread_create` (it imports `wasi.thread-spawn`) exports a mutable i32 global `__stack_limit`.
  A module without the export has no limit. Code that starts no thread, including Swift concurrency's cooperative
  executor, does not link `pthread_create`.
- Each thread runs in its own instance, so the value is per thread: the lowest valid stack pointer of that pthread.
  That is the low end of the `malloc`'ed stack above the guard, or the low end of the buffer passed to
  `pthread_attr_setstack`, and equals `stackaddr - stacksize` from `pthread_getattr_np` (`pthread_attr_getstack`
  returns it as the address).
- `wasi_thread_start` sets `__stack_limit` before `__stack_pointer`, so the limit is in place when the thread's stack
  pointer is first written.
- The main thread, and every thread before `wasi_thread_start` runs, has `__stack_limit` 0, which means no limit.
- A runtime can check every write of `__stack_pointer`: trap when `limit != 0 && new_sp < limit`, with the export looked
  up once per instance.
- The default guard size on `wasm32-unknown-wasip1-threads` is now 8 KiB (it was 0). The guard is not protected; it is
  slack below `__stack_limit`. LLVM lets leaf functions with frames of up to 128 bytes store below the stack pointer
  without writing `__stack_pointer` back, so a check on `__stack_pointer` cannot see those stores; the guard keeps them
  out of the heap. `pthread_attr_setguardsize` now accepts nonzero sizes on this target (up to `SIZE_MAX/8`, as in
  musl; before, it returned `EINVAL` for anything but 0), so the slack can be enlarged and a size read with
  `pthread_attr_getguardsize` can be set again. `pthread_attr_setguardsize(0)` still gives no guard, and
  `pthread_attr_setstack` stacks have none.

`pthread_create` also computes the thread's `stack_size` from the 16-byte aligned stack top now, so that
`pthread_getattr_np` matches the limit exactly (before, the size could be up to 15 bytes too large).

`-4` differs from `-3` in two archives of `WASI.sdk/lib/wasm32-wasip1-threads` only; the compiler, the Swift and
Foundation libraries, module and interface files, the toolset and every other file of the bundle are byte-identical:
- `libc.a`: the code of `wasi_thread_start.s`, `pthread_create.c`, `pthread_attr_setguardsize.c` and `default_attr.c`
  (the 8 KiB default guard).
  The other members that include `pthread_impl.h` were recompiled with the fork's clang (`-3` still had objects
  built by the release clang); they differ in their `producers` section only.
- `libwasi-emulated-signal.a`: `strsignal.c` was recompiled the same way; its `producers` section differs, nothing else.

wasi-libc is a static archive, so code compiled against `-3` gets the limit by relinking against `-4`.

The regression test is `test/src/pthread_stack_limit.c` in the wasi-libc fork commit (registered for the threads
target in `test/CMakeLists.txt`), copied here as `test/stack_limit.c`. `test/stack_limit.sh` is the SDK-level check.

## Changes in -5

`swifthost-sdk-wasip1-threads-6.4.2-5` fixes `_swift_stdlib_immortalize` on WebAssembly (SC-0047). swift-collections 1.7.6
and later call it through `import SwiftShims` to make the empty `Deque` storage immortal. `RuntimeShims.h` declares it as a
plain C function, but `RefCount.cpp` defined it with `SWIFT_CC(swift)`. The WebAssembly backend gives a `swiftcc`
function two extra i32 parameters (`swiftself`, `swifterror`), so the definition had the type `(i32, i32, i32) -> ()`
and the callers' import `(i32) -> ()`. wasm-ld warns "function signature mismatch" (fatal with `--fatal-warnings`);
without the flag it replaces the call with a trap, and the program stops with `unreachable` at the first `Deque`.
Other targets pass the same registers either way, so the mismatch was invisible there.

- The definition now has the C calling convention, as the header says (`RefCount.cpp` includes `RuntimeShims.h`, so the
  compiler checks the two against each other). Swift callers already go through the Clang importer, and swift-collections'
  inlined call is compiled into released packages, so the header side is the one to keep.
- swiftlang/swift#94088 (backported): `setIsImmortal` left the strong extra count at 0, so the first release fell into the
  slow path and the object became mortal again (word `0x800000ff` to `0x7ffffeff` on wasm32). It now keeps count 2
  (`0x800004ff`).
- Audit: every function declared in the 26 installed `SwiftShims` headers is called from C and linked with
  `--fatal-warnings` against all SDK archives. `-4` has one mismatch (this function), `-5` none.

`-5` differs from `-4` in `libswiftCore.a` (one member, `RefCount.cpp.o`) and the two copies of `shims/RefCount.h`. Two
more files differ without a source change: `_Differentiation.swiftmodule` (serialization order is not deterministic) and
`libxml2-export.cmake` (written by a newer cmake). Code built against `-4` is fixed by relinking against `-5`.

The regression test is `test/Runtime/immortalize.swift` in the swift fork commit, and `test/immortalize.swift` here is
the SDK-level reproducer (swift-collections' call through `SwiftShims`, then the refcount word is checked).

## Changes in -6

`swifthost-sdk-wasip1-threads-6.4.2-6` makes every Swift module of the SDK byte-reproducible (SC-0048). The compiler
collected the differentiability witnesses to serialize in a hash set keyed by pointer and wrote them in iteration
order, so their order (and the order of the functions and type IDs they reference) followed heap addresses and changed
from run to run. Only `_Differentiation.swiftmodule` has witnesses, so it was the one SDK module that came out
different on every build; all others were already identical. The compiler now writes the witnesses in insertion order.

- `-6` differs from `-5` in the two copies of `_Differentiation.swiftmodule` only (same size, same content, stable
  record order). Every other file of the bundle, including `libswiftCore.a`, is identical to `-5`.
- Rebuilding the whole SDK from scratch a second time with the same compiler gives a byte-identical bundle and an
  identical zip. The `.swiftinterface` files and the C and C++ libraries were already reproducible.
- The regression test is `test/AutoDiff/Serialization/differentiability_witness_determinism.swift` in the swift fork
  commit (`-emit-module -enable-deterministic-check`).

## Steps (macOS arm64, Xcode 27.2, cmake and ninja on PATH)

```
export W=<empty build directory>
../fetch-src.sh                                 # as for the plain threads SDK
./use-forks.sh                                  # swift, llvm-project and wasi-libc to the fork commits, applies build-system.patch
../build.sh                                     # upstream build-script --build-wasi-stdlib (ends with "build-script exit 1": see below)
../gen-sdk.sh wasm32-unknown-wasip1-threads
./package.sh                                    # writes $W/dist-tailcall/swift-6.4.2-RELEASE_wasm-threads-tailcall.artifactbundle.zip
```

`../build.sh` ends with `build-script exit 1` at the swiftsyntax `swift run` step. That is expected once all WebAssembly
products are built (the just-built toolchain has no `swift-run`).

If `$W` already holds a build of other sources: the standalone stdlib builds do not depend on the compiler binary.
Delete `$W/build/wasm/wasithreadsstdlib-macosx-arm64` and `$W/build/wasm/wasiswiftsdk-macosx-arm64/*/wasm32-unknown-wasip1-threads`
before `../build.sh`, or nothing is recompiled with the new compiler. wasi-libc needs nothing deleted: its build
recompiles the objects whose sources changed.

Differences from the parent recipe:
- The swift, llvm-project and wasi-libc sources and `build-system.patch` described above.
- `package.sh` renames the bundle, artifact and info.json entry to `swift-6.4.2-RELEASE_wasm-threads-tailcall`. The
  triple stays the same, so select the SDK by its id.
- The toolset adds `-Xcc -mtail-call` to `swiftCompiler`, and `-mtail-call` to `cCompiler` and `cxxCompiler`.
- The Embedded variant is removed, as in the parent recipe.

## Using the SDK

- Compile Swift with `-Xcc -mtail-call` and C/C++ with `-mtail-call`. The toolset does this for SwiftPM.
- Code built without the flag is not supported. It links, but calls to `swift_continuation_await`
  (`withCheckedContinuation`, `withUnsafeContinuation`) trap. The caller's own frames also still nest.
- The Swift compiler needs the swift pick #93062, and its LLVM needs #12648, #230073 and #227491. SwiftHostCompiler
  `swifthost-6.4.2-3` and later have all four. The swift.org 6.4.2 toolchain does not: with it, `-Xcc -mtail-call`
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
- `stack_limit.c`: `__stack_limit` on the main thread (0) and on pthreads with default attributes, guards of 0, 8 KiB
  and 64 KiB, 64 KiB and 4 KiB `pthread_attr_setstack` buffers (the 4 KiB one gets its TLS allocated separately), and
  a detached thread, checked against `pthread_getattr_np` and the address of a local (see "Changes in -4");
- `immortalize.swift`: `_swift_stdlib_immortalize` through `import SwiftShims`, then unbalanced releases and 1,000
  retain/release pairs, checking that the refcount word does not change and deinit never runs (see "Changes in -5");
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
- `stack_limit.sh <clang> <threads-sdk-dir> <out-dir> <iwasm> [swiftc]` compiles `stack_limit.c` with both link
  variants, checks with node that `__stack_limit` is an exported global, and runs it. With a `swiftc`, it also builds and
  runs `p1_taskgroup`, `p3_mutex` and a program without threads, and checks that a module exports `__stack_limit`
  exactly when it imports `wasi.thread-spawn`. It prints `stack_limit OK` when everything holds.
- `immortalize.sh <swiftc> <threads-sdk-dir> <out-dir> <iwasm>` builds `immortalize.swift` against the SDK with a plain and
  a `--fatal-warnings` link (no signature-mismatch warning allowed), runs both and prints `immortalize OK` when they
  pass. On `-4` the first link warns, the second fails, and the program traps.
- `validate.mjs <file.wasm>` compiles a module with V8. Its error names the function and the instruction, which is more
  precise than WAMR's loader message.

The probes need an `iwasm` (WAMR 2.4.5) built with `-DWAMR_BUILD_LIB_WASI_THREADS=1 -DWAMR_BUILD_TAIL_CALL=1
-DWAMR_BUILD_FAST_INTERP=1`.
