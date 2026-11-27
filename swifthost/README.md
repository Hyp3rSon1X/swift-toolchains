# SwiftHostCompiler

The Swift 6.4.2 compiler, clang and wasm-ld as a static library for iOS, the iOS simulator and macOS (arm64,
deployment target 27.0). An app links it and compiles Swift source to WebAssembly (wasm32-unknown-wasip1) in its
own process: no subprocesses, no JIT, and no dynamic loading unless the app names a plugin server library. The output
runs in any WASI preview 1 runtime, for example WAMR in interpreter mode.

It compiles against the official swift.org Swift SDK for WebAssembly, `swift-6.4.2-RELEASE_wasm`
(artifactbundle SHA-256 `83baca83553f8c443cda7b84800912e6e709c3acd2e7ebf277794ca5836c0da5`), which the app ships
or downloads itself. Programs can use the standard library, Foundation (FoundationEssentials,
FoundationInternationalization), Observation's runtime, Synchronization, RegexBuilder and the other modules in that
SDK. Macros work through an in-process plugin server that the app provides (see Macros). Since -9 it also compiles
the C targets of packages to WebAssembly objects (`swifthost_clang`), since -10 their C++ targets too (see C and C++
targets).

## Contents

`SwiftHostCompiler-<release>.xcframework.zip`:

| Path | |
|-|-|
| `ios-arm64/libSwiftHostCompiler.a` | iPhone and iPad devices |
| `ios-arm64-simulator/libSwiftHostCompiler.a` | the iOS simulator on Apple silicon |
| `macos-arm64/libSwiftHostCompiler.a` | macOS on Apple silicon |
| `*/Headers/SwiftHostCompiler.h`, `module.modulemap` | the C API; `import SwiftHostCompiler` in Swift |
| `JOBS.txt` | the exact argument lists for compile, autolink and link |
| `README.md` | this file |
| `licenses/` | the license files of everything linked in, byte for byte from the sources (also attached to the release) |

Each `libSwiftHostCompiler.a` holds one prelinked object: the compiler, clang (as the Swift compiler's C importer),
LLVM with only the WebAssembly backend, and lld's wasm linker, linked together (`ld -r`) and with every symbol
hidden except the eight `swifthost_*` functions. LLVM, clang or Swift symbols in the app or in other libraries
cannot clash with it.

## Linking

Add the xcframework to an app target (Xcode: Frameworks, Libraries, and Embedded Content, "Do Not Embed"). The
object carries `LC_LINKER_OPTION`s for everything else it needs, so no other settings are required:

- `libc++`, `libobjc`, `libSystem`: system libraries.
- The Swift runtime from the OS (`/usr/lib/swift`): `libswiftCore`, `libswift_Concurrency`,
  `libswift_StringProcessing`, `libswiftDarwin`, `libswift_DarwinFoundation1/2/3`, `libswift_Builtin_float`.
  The compiler's optimizer is partly written in Swift.
- `libswiftCxx` and `libswiftCxxStdlib`, static libraries from Xcode's toolchain. Xcode passes their directory for
  every target with Swift code. In a target without Swift code, add
  `$(TOOLCHAIN_DIR)/usr/lib/swift/$(PLATFORM_NAME)` to Library Search Paths, or ignore the linker warning:
  no symbols from them are used.
- No zlib, zstd, libxml2 or curl.

Size: each `libSwiftHostCompiler.a` is about 209 MB (182 MB in `swifthost-6.4.2-1`, before swift-syntax was added for
macros). Linked into the macOS smoke test with dead stripping, it adds about 110 MB to the executable: `__TEXT`
104 MB, `__DATA_CONST` 3.0 MB, `__DATA` 2.2 MB.

## Using it

See `SwiftHostCompiler.h` for the contract and `JOBS.txt` for the jobs. In short:

1. Run every call on a thread with at least `SWIFTHOST_MIN_STACK_SIZE` (16 MiB) of stack, not on the main thread.
2. Compile with `swifthost_swift_frontend`, extract the autolink entries with `swifthost_swift_autolink_extract`,
   link with `swifthost_wasm_ld`; compile the C and C++ sources of package C and C++ targets with `swifthost_clang`. Each takes an argv and two file descriptors (output, diagnostics) and returns the
   tool's exit status.
3. Pass `-module-cache-path` to a writable directory that survives between builds (for example in Caches).
4. If a call returns `SWIFTHOST_EPOISONED` (125), the compiler crashed in this process (in this call or an earlier
   one): tell the user to restart the app before compiling again.
5. To stop a compile, call `swifthost_request_cancel()` from another thread; the frontend call returns
   `SWIFTHOST_ECANCELED` (124) and the library stays usable.

Measured with `test/smoke.c` on macOS on a Mac mini (Apple M4) shared with other builds:

| | first build (empty module cache) | later builds |
|-|-|-|
| hello world, `-Onone`: frontend | 0.1-0.3 s (includes building the WASILibc and SwiftShims clang modules) | 15-50 ms |
| hello world: wasm-ld | 0.1-0.15 s | 75-120 ms |
| Foundation program, `-Onone` / `-O`: frontend | 0.65-1.1 s | 40-180 ms |
| Foundation program: wasm-ld | 0.3-0.7 s | 0.3-0.4 s |

Peak memory (resident) of the whole process: about 180 MiB after hello world builds, 440-500 MiB after Foundation
builds. Stack: the calling thread used 80 KiB for these programs (clang module builds run on their own 8 MiB helper
thread); `SWIFTHOST_MIN_STACK_SIZE` is headroom for deeply nested source. `swifthost_request_cancel()` stopped a long
type check within 2-8 ms, among top-level statements, among function bodies and (since `-5`) inside one long
function body.

In the iOS 27.2 simulator on the same Mac, in two runs while other builds were using the Mac, every step was 5-100x
slower (hello world 1.3-9 s for the first build and 0.03-0.6 s later, Foundation frontend 0.8-8.8 s, wasm-ld 0.6-7.4 s,
cancel within 0.08-1.4 s; peak memory about the same). We did not find out why; processes started with `simctl spawn`
may run at a low scheduling priority. Measure on a device.

## Macros

The compiler includes swift-syntax and expands macros like `swiftc` does with an in-process plugin server: it sends
each request (`getCapability`, `loadPluginLibrary`, `expandFreestandingMacro`, `expandAttachedMacro`) as a JSON
message to a function and gets the reply back from it. The library contains no macro implementations; the app runs
them (for example as WebAssembly plugin modules in its own runtime) and answers the messages. Two ways:

- `swifthost_set_plugin_message_handler(handler)`, once, before the first compile: the app's own function receives
  every message (same contract as `swift_inproc_plugins_handle_message`; the reply is a `malloc`ed buffer that the
  compiler frees). `-in-process-plugin-server-path` is then optional; a path given there is never opened.
- `-in-process-plugin-server-path <library>`: a dynamic library that exports `swift_inproc_plugins_handle_message`,
  loaded by the library (macOS; not for App Store iOS apps).

The release also carries `swift-builtin-macros-6.4.2-wasm32-wasip1.wasm`: the built-in macro plugins
(ObservationMacros, SwiftMacros, TestingMacros, FoundationMacros) from the `swift-6.4.2-RELEASE` sources as one
WebAssembly (WASI command) module using swift-syntax's wasm plugin ABI (`_start`, then `swift_wasm_macro_v1_pump`
per message, 8-byte little-endian length framing on stdin/stdout); load the same file under each module name. Its
recipe, test host and provenance are in `macros/` of the swift-toolchains repository.

Each macro module is named with `-load-resolved-plugin <file>##<Module>`; the server gets the file's resolved path
in `loadPluginLibrary`. See `JOBS.txt` for the arguments and message formats. A malformed or hostile reply is an
error diagnostic for that macro (return value 1) and the library stays usable. Before the compiler parses a reply,
the library rejects one that nests arrays or objects more than 512 levels deep (swift-syntax's JSON parser recurses
once per level and would overflow the stack at around 100,000). The handler usually runs on the calling thread, but
also on the compiler's 8 MiB helper thread when a module is rebuilt from its `.swiftinterface`; a crash in the
handler there ends the process. Executable plugins (`-load-plugin-executable`, `-external-plugin-path`) are compiled
out: they fail with "executable plugins are not supported in this build", and no process is started.

## Async code and WebAssembly tail calls

Swift 6.4.2 compiles async functions for WebAssembly without tail calls: an `await` that completes without
suspending leaves a frame on the WebAssembly stack until the task next suspends. A long run of such awaits, for
example a loop that awaits a function that never suspends, overflows the stack (after 1,000 to 6,000 awaits with a 1 MiB
stack).

From `swifthost-6.4.2-3`, compile with `-Xcc -mtail-call` (the `tail-call` WebAssembly feature). Async functions then
use the `swifttailcc` calling convention, every continuation call is a guaranteed tail call (`return_call`), and the
stack stays flat. The program must be linked against a Swift SDK whose libraries are built the same way, such as
`swifthost-sdk-wasip1-threads-6.4.2-3` (or `-2`) from this repository (artifact id `swift-6.4.2-RELEASE_wasm-threads-tailcall`,
whose toolset adds the flag), and the runtime must support the WebAssembly tail-call proposal (WAMR: built with
`WASM_ENABLE_TAIL_CALL=1`). Use `-Xcc -mtail-call` only with this compiler or later: `swifthost-6.4.2-2` and the
swift.org 6.4.2 compiler do not use tail calls for async code even with the flag. Do not mix: code compiled without
the flag links against that SDK, but calls into the runtime such as `withCheckedContinuation` trap, and deep chains of
awaits still overflow the stack.

Without the flag, the compiler generates the same code as `swifthost-6.4.2-2`, except that the `swift_task_switch`
and `swift_asyncLet_finish` declarations carry the `swiftasync` attribute.

## C and C++ targets

`swifthost_clang` (since -9; C++ since -10) compiles one C or C++ source file per call to a WebAssembly object with
the library's clang, for the C and C++ targets of packages (for example `_NumericsShims`, `_AtomicsShims`,
swift-markdown's `CAtomic` and `cmark-gfm`, swift-syntax's `_SwiftSyntaxCShims`; in C++ `_FoundationICU`, zxing-cpp's
`ZXingCppCore`, `simdjson`, `immer`). It takes clang driver arguments for exactly one compile job, as SwiftPM's native
build system passes them (SwiftPM runs `clang` for C++ files too; `clang++` works as well), plus `-resource-dir` (the
SDK's `swift_static/clang`) and the toolset's flags; `JOBS.txt` has the full argument list under "C and C++ compile
jobs" and the header the contract. With the same sources and flags the objects are byte-identical to the stand-alone
clang's. Accepted are C, C++ and their preprocessed forms; headers, C++20 module units, Objective-C(++), assembler and
LLVM IR inputs, clang and C++20 modules (options and module pragmas or imports in the source),
precompiled headers, options that spawn, load code or write to the process's standard streams,
and commands that need more than one job are rejected with one error line. A running call is cancelled by
`swifthost_request_cancel()` like a frontend call, in C++ also while templates are instantiated.

C++ targets reach Swift through a C header (`extern "C"`), like C targets; Swift's C++ interoperability
(`-cxx-interoperability-mode`) is not covered. The app must ship the SDK's `WASI.sdk/include/c++/v1` (libc++'s
headers, found through `--sysroot`) and link `-lc++ -lc++abi`, which the Swift link line already does. The SDK's
libc++abi has no exception runtime, while clang turns C++ exceptions on by default: a target that throws, or uses a
throwing part of libc++ (`std::map::at`, `std::function`, ...), links only when compiled with `-fno-exceptions`.

## Changes in swifthost-6.4.2-10

- `swifthost_clang` compiles C++ too (see C and C++ targets): C++ and preprocessed C++ inputs, argv[0] `clang++` and
  `--driver-mode=g++` besides `clang`, libc++ from the SDK through `--sysroot`. No compiler change: clang's C++ front
  end was already in the library. Still rejected: headers, header units and C++20 module units, and now for C and
  C++ alike every clang-modules and C++20-modules option (`-fmodules`, `-fmodule-*`, `-fmodules-*`,
  `-fimplicit-module-maps`, `-fprebuilt-module-path=`, `--precompile`, ...) and the driver modes other than gcc and g++.
  Module requests in the source fail with one error line, without writing a file: `#pragma clang module build` (which
  compiled a nested module into a `.pcm` file in `$TMPDIR`, outside the call's cancellation) and module imports
  (`import`, `@import`, `#pragma clang module import`/`load`). Precompiled headers (`-include-pch`, or an `-include`
  whose `.pch`/`.gch` file exists) are rejected.
- Cancellation in C++: a cancel during template instantiation, inside a declaration or at the end of the file, now
  stops all further instantiation (the call reports the cancel as before and also puts clang's diagnostics into the
  state after a fatal error, in which Sema instantiates nothing more). Measured median 14-18 ms from the request to
  the return (CPU time) while 250 `std::map` specializations are instantiated at the end of a file, where the C-only
  checks of -9 took 1.7-1.9 s (up to 2.8 s). The IR generation at the end of a C++ file still runs to its end (up to
  about 0.5 s measured).
- Tested with the C++ targets `_FoundationICU` (swift-foundation-icu swift-6.4.2-RELEASE, 476 files), `ZXingCppCore`
  (zxing-cpp v3.2.0, 104 C++ files and 1 C file), `simdjson` (simdjson-spm 4.6.4), `immer` (v0.9.1) and a reference
  package (4 files): SwiftPM 6.4.2's lines for the `swifthost-sdk-wasip1-threads-6.4.2-6` SDK, debug and release
  (1,174 compiles), three passes in one process, the third interleaved with the 62 C lines of -9 and a Swift
  compile: objects and dependency files byte-identical to the stand-alone clang built from the same sources. The
  reference package's Swift program (global constructors, RTTI, containers, `std::thread`) and an ICU caller
  (`u_strToUpper`, `ucol_strcoll`) link in-process and run in WAMR. ICU's data file `icu_packaged_data.cpp` takes
  about 17 s and a peak of about 8 GB of memory to compile, in-process as with the stand-alone clang.
- The Swift compiler, sourcekitd, wasm-ld and the C path are unchanged from -9 (same fork commits, same objects for
  the C lines).

## Changes in swifthost-6.4.2-9

- New entry point `swifthost_clang`: an in-process C compiler for the C targets of packages (see C targets). It needs
  no compiler change: the clang driver, code generation and the WebAssembly backend were already in the library for
  the Swift compiler's C importer. Each slice grows by about 0.1 MB.
- Cancellation of a clang call: at every top-level declaration while parsing, before every optimization pass, and in
  code generation at its next function, pass or block. Measured cancel-to-return latency (CPU time): medians of
  10-40 ms while parsing and in code generation, 100-170 ms in optimization, up to about 0.5 s when one
  optimization pass works on one function of 20,000 statements. A cancelled call returns 124 with the one diagnostic
  `compilation cancelled` and writes no object; it does rewrite the `-MD` dependency file (see the header).
- Tested with the 31 C files of the five targets above (SwiftPM 6.4.2's lines for the
  `swifthost-sdk-wasip1-threads-6.4.2-6` SDK, debug and release), three passes in one process: objects and dependency
  files byte-identical to the stand-alone clang built from the same sources; against the swift.org-style toolchain
  clang only the embedded version string differs. A Swift program that calls into all five, compiled and linked
  in-process, runs in WAMR.
- The Swift compiler, sourcekitd, wasm-ld and their output are unchanged from -8: same fork commits, same API for
  the other entry points, and the macro module and swift-syntax version are the same.

## Changes in swifthost-6.4.2-8

- Very large functions at `-O` no longer need gigabytes. LLVM's WebAssembly pass for irreducible control flow
  (`WebAssemblyFixIrreducibleControlFlow`) built a reachability graph that grew with the square of the number of
  blocks. It is now upstream's SCC-based version (llvm/llvm-project#184441, backported). One function of 4,000
  `if` and `?:` statements at `-O` was killed at 6.5 GB with `-7`; it now completes with a peak of about 1.1-1.7 GB
  (1,000 statements: 302 MiB), in about 72 s of CPU time.
- A cancel now also stops LLVM code generation (instruction selection, register coalescing and allocation, the
  WebAssembly passes, the writing of the object), at its next block, instruction, function or pass. Before, it ran
  to the end first: for one function of 4,000 statements, seconds to half a minute of CPU time and the memory with it.
  Typical cancel-to-return latency in code generation is 0.06 s of CPU time (90th percentile 0.12 s, longest 0.53 s
  measured). Still uninterruptible: one running LLVM optimization pass, CodeGenPrepare, and one SIL pass on one giant
  function (see the header). A cancelled call still leaves no object file and reports one diagnostic.
- At `-Onone`, wasm32 functions no longer get one local per temporary. Above a threshold of 4,096 32-bit cells of
  params and locals per function (hidden option `-Xllvm -wasm-o0-reg-coloring-threshold=<cells>`; 0 colors every
  function), the backend colors the locals as at `-O`; smaller functions are unchanged, byte for byte. The function of
  4,000 statements went from 120,008 cells to 13, so its module loads in WAMR's fast interpreter ("local count too
  large" before). The fast interpreter also gives every distinct constant of a function a cell of its stack frame:
  such a module needs a WAMR wasm stack of at least 256 KB (the default of iwasm is 64 KB).
- Uncancelled output is the same as with `-7`, except the 16-byte module hash (it covers the LLVM revision) and
  functions of more than 4,096 cells at `-Onone`. The fork commits since `-7` are listed under Source and patches.
  The API, `JOBS.txt`, the swift-syntax version and the built-in macro module are the same as in `-7`.

## Changes in swifthost-6.4.2-7

- A cancel during SIL simplification no longer aborts the process. In `-6`, a cancel that landed in the
  simplification of a function (`OnoneSimplification` and the other simplification passes, also at `-O`) could stop
  it before its end-of-pass fix-ups. The pass manager then failed an always-on assertion
  (`didn't break infinite loops`, `PassManager.cpp`, signal 6) and ended the app's process, about 2 times in 1,000
  cancels in an iOS app. Simplification, SILCombine and SimplifyCFG now run their dead-code clean-up and fix-ups
  also when a cancel stops them.
- A cancelled compile no longer asserts on pass invariants (infinite loops unbroken, lifetimes incomplete, stack
  nesting not fixed): while the compile is cancelled, they are cleared instead. Nothing changes without a pending
  cancel, and the assertions fire as before in a compile that is not cancelled. The hidden frontend option
  `-Xllvm -sil-cancel-keep-invariant-asserts` keeps the assertions in cancelled compiles, to check that a pass
  restores its invariants itself.
- Uncancelled output is the same as with `-6`. The four swift fork commits since `-6` are listed under Source and
  patches. llvm-project, swift-syntax, the API, `JOBS.txt` and the built-in macro module are the same as in `-6`.

## Changes in swifthost-6.4.2-6

- Sendable checking (region-based isolation) no longer takes memory and time that grow with the square of one
  function's size. It kept, for every basic block, the region of every value tracked so far; those copies now share
  everything they have in common. One function of 1,000 statements with closures (`-Onone -swift-version 6`,
  in-process on macOS): 6.2 s of CPU time and a 349 MB peak footprint instead of 14.5 s and 2.7 GB, of which
  sendable checking 0.2 s instead of 5.7 s. 2,000 and 4,000 statements: 20.7 s / 1.0 GB and 48.9 s / 2.9 GB, where
  `-5` went past 5 GB; the peak now comes from code generation, and sendable checking stays below 340 MB.
  Diagnostics and objects are the same as with `-5`.
- A cancel inside the sendable checking of such a function returns within 25-75 ms of CPU time (with `-5`, freeing
  its GBs alone took 0.6-0.9 s).
- Still slow, as with `-5`: one function whose loops merge regions on every iteration, for example thousands of
  `for` loops that each link a new object to the previous one. Each merge of two regions walks every tracked value
  of the function, so the time grows with about the cube of the number of loops: sendable checking takes 1-3 s
  for 1,000 such loops (`-5`: 10 s), about 40 s and 1.2 GB for 2,000 (`-5`: 81 s, 3 GB), and goes past 5 GB for
  4,000 (as with `-5`).
- The five swift fork commits since `-5` are listed under Source and patches. llvm-project, swift-syntax, the API,
  `JOBS.txt` and the built-in macro module are the same as in `-5`.

## Changes in swifthost-6.4.2-5

- `swifthost_request_cancel()` no longer waits for the end of the stage it lands in: type checking stops at the
  next statement of a function body (one function of 4,000 statements, cancelled after 300 ms of the call's CPU time,
  returns after about 3 ms of CPU time instead of 2.8 s, with one diagnostic instead of about 4,000), SIL generation
  at the next statement, the SIL passes at the next pass or inside the long ones (sendable checking, ARC optimization,
  copy propagation, Mem2Reg), IR generation at the next function, and LLVM before code generation. What still runs
  to the end is listed in the header. Before it returns, a cancelled call frees what it built; for the sendable
  checking of a function of thousands of statements that can be GBs, at about a quarter of a second of CPU time
  per GB. Uncancelled compiles produce the same objects as with `-4`.
- A cancel that landed in SIL generation could crash the call with `-4` (for example "Assertion failed: (init),
  function emitApplyAllocatingInitializer", or a signal), after which every later call returned 125: SIL generation
  type-checks synthesized code (derived `Equatable`, `Hashable`, `Codable` and `RawRepresentable` members) on demand,
  and went on with what the cancelled type check had left. Such code now becomes an error body, and SIL generation
  stops at its next statement, also inside expressions such as string interpolations. The same held for caller-side
  default arguments (`#file`, `#line`, `nil`, `[]`, `[:]`), which the type checker (after each body) and SIL
  generation (at each call) type-check on demand: a cancelled one could fail the assertion that an error was reported
  ("Assertion failed: (ctx.Diags.hadAnyError())"). It now becomes an error expression.
- A cancelled call reports one diagnostic of its own, `<unknown>:0: error: compilation cancelled` (diagnostic ID
  `compilation_cancelled`, see the header), and nothing after it. With `-4`, most cancelled builds also reported
  "the compiler is unable to type-check this expression in reasonable time" for the expressions the type checker
  gave up on, and a cancel after type checking reported nothing.
- A cancelled call writes no object or IR file (with `-num-threads`, those written before the cancel are removed);
  outputs written before the cancel, such as the module or SIL, may exist. Ignore every output of a cancelled call.
- sourcekitd: the code completion after a cancelled one could reuse the cancelled one's half-checked AST and
  return no results; a cancelled operation now leaves nothing for reuse.
- The fourteen swift fork commits since `-4` are listed under Source and patches. llvm-project, swift-syntax, the API,
  `JOBS.txt` and the built-in macro module are the same as in `-4`.

## Changes in swifthost-6.4.2-4

- The object is built with `SOURCEKIT=1`: sourcekitd's C API (191 entry points of `sourcekitd.h`, in-process, no XPC)
  is exported next to the seven `swifthost_*` functions (198 exports per slice), with crash containment and
  `swifthost_sourcekitd_status()` (see "In-process sourcekitd" below and in the header). The library is about 3.4 MB
  larger per slice.
- New frontend flag `-platform-condition-os <value>`: it overrides only the value that `os()` checks.
- The nine swift fork commits since `-3` are listed under Source and patches. The llvm-project branch, swift-syntax,
  the compiler's behavior without these changes, `JOBS.txt` and the built-in macro module are the same as in `-3`.

## Changes in swifthost-6.4.2-3

- WebAssembly tail calls for async code with `-Xcc -mtail-call` (see above): four upstream changes, three in
  llvm-project and one in swift, plus the swift tests for them (see Source and patches). Nothing else changed:
  swift-syntax, the API, `JOBS.txt` and the built-in macro module are the same as in `-2`.

## Changes in swifthost-6.4.2-2

- Macros (see above): swift-syntax is linked in, `swifthost_set_plugin_message_handler()` is new, executable plugins
  are rejected.
- Diagnostics use swift-syntax's style, which prints source excerpts under each `file:line:column: error: ...` line;
  `-diagnostic-style llvm` gives the one-line diagnostics of `-1`.
- The compiler's regex literal parser is built in (the build enables it only with swift-syntax; `-1` was built
  without it).
- The library is about 27 MB larger per slice.
- New release asset: the built-in macros as one wasm32-wasip1 module (above), with its own SHA-256 sums, provenance
  and license files.

## Trimming the SDK

The jobs in `JOBS.txt` read only part of the `wasm32-unknown-wasip1` directory (338 MiB, 3,324 files). With these
removed, the smoke test still passes (164 MiB, 1,125 files left):

- `swift.xctoolchain/usr/lib/swift` (148 MiB): the resource directory for dynamic linking; the jobs use
  `swift_static` (`-use-static-resource-dir`).
- every `*.swiftinterface`: the binary `.swiftmodule`s load directly, and no module is rebuilt into the module cache.
- every `*.swiftdoc` (documentation comments; opened when present, not needed).
- `WASI.sdk/include/c++` (libc++ headers), but only if the app compiles no C++ with `swifthost_clang` (since -10;
  the smoke test's C++ case needs it, and `test/sdk-access-smoke.txt` was recorded before that case existed).

`test/sdk-access-smoke.txt` lists every SDK file that the smoke test's programs opened or checked for (recorded with
`test/fsaccess.c`). Keep files that are only checked for (`probe`), such as headers named in module maps: clang
looks them up while building a module. A program that imports a module the smoke test does not use (RegexBuilder,
Synchronization, ...) needs that module's `.swiftmodule` directory and its `lib*.a` as well.

## In-process sourcekitd (SOURCEKIT=1)

Built with `SOURCEKIT=1`, the object also exports sourcekitd's C API (`sourcekitd.h`, in-process, no XPC service),
which shares the process with the compiler calls; see "In-process sourcekitd" in `SwiftHostCompiler.h` for the
contract. In short:

- A crash or LLVM fatal error in sourcekitd's work stops sourcekitd instead of ending the process: pending and later
  requests get an error response, `swifthost_sourcekitd_status()` turns non-zero, and only a process restart starts
  sourcekitd again.
- After such a stop, compiler calls keep working on a best-effort basis only: if the crashed thread held shared
  process state (a C++ static initialization guard, a `swift_once`, the malloc lock, a lock of the Swift runtime's
  caches, a clang module's lock file), later compiler calls may block or fail. Restart the app process at the next
  convenient point after a recovered crash.
- The app's response receivers and notification handler run outside the library's crash recovery: a crash there is
  the app's and ends the process. The app's plugin message handler runs inside sourcekitd's work: a crash there
  stops sourcekitd. Calls into the app's receivers are held until the work item that produced them ends: if that
  work item crashes later on, a response it had already finished is dropped and the request gets the stop error
  instead. Notifications that were already queued are still delivered after sourcekitd stopped.
- The library's attribution handler sits only directly over LLVM's crash recovery handler. A signal handler the app
  installs while crash recovery is enabled is left alone; a fault it deals with itself affects nothing.
- `SWIFTHOST_TEST_SOURCEKIT_RECOVERY_DELAY_MS` is a hook for the library's own tests. Never set it in production:
  it delays the stop after a recovered crash and stalls frontend calls (up to 5 s at their start and end each, after
  which they may be poisoned).
- Every `swifthost_wasm_ld` call resets LLVM's process-wide options while sourcekitd may read them; do not pass
  `-Xllvm`/`-mllvm` while sourcekitd is in use.

## Limits

- One call at a time per process (calls are serialized internally).
- `swifthost_clang` compiles C and C++ (no Objective-C, no headers or C++20 module units), one source file per
  call; C++ code that throws needs an exception runtime the SDK does not have (see C and C++ targets).
- No macro implementations are included (`@Observable`, `#Predicate`, `#expect`, ...): the app serves them (see
  Macros). A macro whose module no plugin serves is an ordinary compile error (return value 1), for example:
  `error: external macro implementation type 'ObservationMacros.ObservableMacro' could not be found for macro
  'Observable()'; plugin for module 'ObservationMacros' not found`.
- After a compiler crash the library refuses further work until the process restarts (the crash recovery skips
  destructors, so the compiler's global state cannot be trusted). That includes crashes on the compiler's helper
  threads (clang module builds), which the compiler itself reports as an ordinary error, a stack overflow on the
  calling thread, and calls that end in LLVM's exit path, such as `-Xllvm -version` or an unknown `-Xllvm` option
  (whose output goes to the process's stdout/stderr). Such a call never returns 0 or 124.
- While a call runs, LLVM's crash recovery owns the SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV and SIGTRAP handlers,
  and the calling thread runs with an alternate signal stack of the library's own. A crash on another thread is
  passed on to the handlers that were installed before the call, but without its fault details, and crash recovery
  stays off for the rest of that call; see the header.
- A stack overflow on a helper thread (8 MiB) cannot be recovered and ends the process.
- Sendable checking of one function whose loops merge regions on every iteration still grows faster than the
  function (see Changes in swifthost-6.4.2-6).
- A cancel does not stop one running LLVM optimization pass, CodeGenPrepare or one SIL pass on one giant function; it
  lands when that pass ends (see the header).
- At `-Onone`, a module with a very large function (thousands of statements) needs a WAMR wasm stack of at least
  256 KB (see Changes in swifthost-6.4.2-8).

## Source and patches

Built from public forks, on top of Swift 6.4.2:

| Repository | Branch | Commit |
|-|-|-|
| [Hyp3rSon1X/swift](https://github.com/Hyp3rSon1X/swift) | `hyp3r/ios-host/6.4.2` | `fce936f5d677135d99c869ae8d36979343e7a65b` |
| [Hyp3rSon1X/llvm-project](https://github.com/Hyp3rSon1X/llvm-project) | `hyp3r/ios-host/6.4.2` | `6d8c598c2be47e43ebe2939f33678b184bfaf965` |
| swiftlang/swift-cmark | tag `swift-6.4.2-RELEASE` | `924936d0427cb25a61169739a7660230bffa6ea6` |
| swiftlang/swift-experimental-string-processing | tag `swift-6.4.2-RELEASE` | `7581e7b803d2ccc58409e45e1bfde40146cb8e41` |
| swiftlang/swift-syntax | tag `swift-6.4.2-RELEASE` | `050f1a346fbbac0ca2cfb15a95274f7bd1cf0ccf` |

The swift branch is `swift-6.4.2-RELEASE` plus the compiler fixes of `swift-6.4.2-hyp3r.7` in this repository
(see `releases/6.4.2-hyp3r.7.env`), plus these changes for running in-process:

- `[Frontend]` `performFrontend` and `autolink_extract_main` overloads that take the diagnostic stream, so each call
  writes to its own file descriptor instead of the process's stderr.
- `[SIL]` deserialization, linking and debug-option failures exit through `llvm::sys::Process::Exit` (which crash
  recovery turns into a return) instead of `exit()`.
- `[AST]` no reference to the swift-syntax `StaticBuildConfiguration` in builds without swift-syntax.
- `[Sema]` type checking of a file stops at the next top-level declaration or function body once the host cancels
  the compilation (`ASTContext::CancellationFlag`), nothing else runs on the file after that (the playground, PC
  macro and debugger testing transforms crashed on the unchecked code), and the frontend skips the work after
  semantic analysis; before, a cancelled compile finished type checking every top-level statement and function
  body first.
- `[CMake]` `SWIFT_COMPILER_SWIFT_SYNTAX_STATIC` (default off): the compiler's swift-syntax libraries as static
  libraries, so they are linked into the prelinked object.
- `[AST]` `InProcessPlugins::setHandleMessageOverride()`: a process-wide handler that replaces the in-process plugin
  server library (behind `swifthost_set_plugin_message_handler`).
- `[AST]` `SWIFT_DISABLE_EXECUTABLE_PLUGINS` (CMake option, default off): executable plugins are rejected with an
  error instead of launched.
- `[ASTGen]` diagnostic positions from a plugin that lie before the source buffer, and ranges that end before they
  start, are ignored instead of reaching the source manager.
- `[IRGen]` (from `swifthost-6.4.2-3`) cherry-picks of swiftlang/swift `d7dcf221748` (tests for WebAssembly tail calls
  in async code) and `3c9966afca9` (the `swift_task_switch` and `swift_asyncLet_finish` declarations carry the
  `swiftasync` attribute of the runtime's definitions; without it, their WebAssembly signatures differ from the
  runtime's when tail calls are enabled, and every call to them is invalid). Neither is in Swift 6.4.x.
- `[Frontend]` (`091796f90b9`) `-platform-condition-os` overrides the `os()` condition.
- For the in-process sourcekitd next to the compiler calls (`SOURCEKIT=1`):
  - `[SwiftCompilerSources]` (`90e548eb1f7`) `initializeSwiftModules()` is idempotent and thread-safe (sourcekitd
    and the frontend both call it; the second registration aborted the process).
  - `[Basic]` (`167a178b3f7`) LLVM's targets are initialized once per process (`swift::initializeLLVMOnce()`).
  - `[FrontendTool]` (`7bcf8acf27a`) `FrontendObserver::shouldInstallFatalErrorHandler()`: a host keeps its own LLVM
    fatal error handler during `performFrontend`.
  - `[AST]` (`4e10b19c6b1`) the host's in-process plugin message handler gets each message's server path.
  - `[SourceKit]` (`ede680d5e80`) `WorkQueue::setExecutionHook()`: the host runs SourceKit's work items (under its
    crash recovery).
  - `[sourcekitd]` (`6c805f13653`) `sourcekitdInProc::setServiceHooks()`: the host interposes on requests and can
    turn the service off after a crash.
  - `[sourcekitd]` (`29649a8d637`) `ServiceHooks::DeliverNotification`: the host calls the notification handler
    (outside its crash recovery).
  - `[sourcekitd]` (`043f6938c4b`) `ServiceHooks::ShouldInstallFatalErrorHandler`: `sourcekitd_initialize` keeps the
    host's LLVM fatal error handler.
- Finer cancellation (from `swifthost-6.4.2-5`; each stage checks `ASTContext::CancellationFlag` at its statement,
  function or pass boundaries, so a cancel no longer waits for the end of the stage):
  - `[AST]` (`285064fad14`) `ASTContext::isCancellationRequested()`.
  - `[Sema]` (`18aa4d6f544`) the type checker stops at the next statement of a body and before the next expression,
    leaves an error body behind (like a failed result builder) and reports one "compilation cancelled" error.
    Before, it checked every remaining statement of the body (each one failing as "too complex").
  - `[IDETool]` (`6068fcb184a`) a cancelled code completion or compile does not leave its compiler instance for
    reuse. Before, the completion after a cancelled one could reuse a half-checked AST and return no results.
  - `[SILGen]` (`9fe9b8aca87`) SIL generation stops at the next statement or declaration (inside an expression,
    such as a string interpolation, it goes on in an unreachable block).
  - `[SILOptimizer]` (`619e4100ff6`) the pass manager runs no further pass and the passes with sub-pass bisection
    stop at their next sub-pass; nothing runs on the module after a cancelled pipeline, and no lowered SIL is written.
  - `[SILOptimizer]` (`66e278d2aee`) region analysis (sendable checking) stops at its next block.
  - `[SILOptimizer]` (`4977df53e98`) the sendable checking's diagnostic walk (SendNonSendable) stops at its next
    block and emits nothing.
  - `[SILOptimizer]` (`f90f8a68441`, `b2e93fae953`) SemanticARCOpts and Mem2Reg stop at their next worklist item.
  - `[IRGen]` (`547fc71f131`) IR generation stops at the next function and returns no module.
  - `[IRGen]` (`69250f0a024`) LLVM skips its remaining optional passes and stops before code generation, without
    leaving an object file (`IRGenOptions::CancellationFlag`). Code generation stops too from `swifthost-6.4.2-8`.
  - `[Frontend][Sema]` (`cc0ed4e3825`) a cancelled compilation reports exactly one diagnostic, the fatal error
    "compilation cancelled" (no "too complex" errors from the solver, nothing reported after the cancel).
  - `[Sema][SILGen]` (`7721e874127`) code that the type checker and SIL generation type-check on demand keeps the
    cancellation error: a cancelled caller-side default argument becomes an error expression instead of being
    retried at its declaration (the retry dropped the error), and a synthesized body is checked for a cancellation
    before it is taken as type-checked. SIL generation also accepts an error expression once cancelled.
  - `[IRGen][Frontend]` (`941d248677b`) one check decides about a cancellation for each output: with `-num-threads`
    the one after the code generation threads, which removes the object and IR files they wrote; with one module the
    frontend's, before the IR file is written. IR generation reports a cancellation as an error.
- Memory and time of sendable checking on large functions (from `swifthost-6.4.2-6`); region labels, diagnostics
  and objects stay the same:
  - `[SILOptimizer]` (`2353b191b9e`) isolation history is recorded only if its factory was built with recording on;
    the region analysis builds it with recording off (nothing in 6.4.2 rewinds it).
  - `[SILOptimizer]` (`fead368f62c`) a partition's element-to-region map is a persistent radix trie
    (`PersistentIndexMap`): copying it is O(1), and a change copies only the nodes on its path.
  - `[SILOptimizer]` (`63cf97fdeed`) joining into a partition without elements shares the other side's map.
  - `[SILOptimizer]` (`ef76a76bee8`) canonicalization relabels only the regions created since the partition was last
    canonical.
  - `[SILOptimizer]` (`557485911d1`) the trie's leaves record which slots are occupied, so a walk costs the number of
    entries, not the range of element numbers.
- Cancel during SIL simplification (from `swifthost-6.4.2-7`); without a stop, nothing changes:
  - `[SILOptimizer]` (`7c9a631b72e`) the simplification pass removes dead code and does its fix-ups
    (`fixStackNesting`, `breakInfiniteLoops`, `completeLifetimes`) also when it is stopped early.
  - `[SILOptimizer]` (`bbb7c5cf113`) SILCombine reports the changes of an iteration that was stopped early.
  - `[SILOptimizer]` (`412d022e93e`) SimplifyCFG removes the unreachable blocks once at the end of a stopped run and
    includes the current iteration's changes in `dominatorBasedSimplify`.
  - `[SILOptimizer]` (`529a4e2687c`) fork only: while the compile is cancelled, the pass manager clears the pass
    invariants instead of asserting on them; the hidden option `-sil-cancel-keep-invariant-asserts` keeps the
    assertions.
- Cancel in code generation (from `swifthost-6.4.2-8`):
  - `[IRGen]` (`fce936f5d67`) fork only: the compilation's cancellation flag is attached around the LLVM code
    generation pass manager, and the temporary output is discarded when the compile was cancelled.

The llvm-project branch is `swift-6.4.2-RELEASE` plus:

- `[Support]` `LLVM_ENABLE_SIGNAL_HANDLERS` (CMake option, default on): when off, LLVM never installs its
  process-wide handlers for SIGINT, SIGTERM, SIGHUP, SIGUSR1/2, SIGINFO and the others.
- `[Support]` `report_fatal_error`, command-line option errors, `-help`/`-version` and LLVMContext's default
  diagnostic handler exit through `sys::Process::Exit` instead of `exit()`.
- `[Support]` `CrashRecoveryContext::getNumRecoveredCrashes()`, a process-wide count of the crashes that crash
  recovery contexts recovered from on any thread, so the library notices a crash that the compiler recovered from
  on a helper thread and reported as an ordinary error.
- `[Support]` the crash recovery signal handlers are installed with `SA_ONSTACK`, so a thread with an alternate
  signal stack recovers from a stack overflow instead of dying.
- `[WebAssembly]` (from `swifthost-6.4.2-3`) a cherry-pick of swiftlang/llvm-project `b9c55a400656`, "Wasm: support
  tail calls in CallingConv::SwiftTail", which is not in Swift 6.4.x: with the `tail-call` target feature, clang
  accepts `swiftasynccall` for WebAssembly, and the backend lowers `swifttailcc` `musttail` calls to `return_call`.
- `[WebAssembly]` (from `swifthost-6.4.2-3`) a cherry-pick of llvm/llvm-project `fd9bc2e181b7`: a tail call in a
  function with a stack protector is emitted after the guard check and after the stack pointer is restored. Before,
  it was emitted ahead of both.
- `[WebAssembly]` (from `swifthost-6.4.2-3`) a cherry-pick of llvm/llvm-project `38e6c0073c42`: a call with a `byval`
  argument is never lowered as a tail call. Before, with tail calls enabled, the callee could get a pointer into the
  caller's released stack frame (for example C++ thunks that forward `std::string_view` by value).
- `[WebAssembly]` (from `swifthost-6.4.2-8`, `175078457d42`) a cherry-pick of llvm/llvm-project `0a76568db07e`
  (#184441): `WebAssemblyFixIrreducibleControlFlow` works on strongly connected components instead of a quadratic
  reachability graph.
- `[WebAssembly]` (from `swifthost-6.4.2-8`, `8acf109c7168`) fork only: at `-O0`, functions with more than 4,096
  cells of params and locals are register-colored as at `-O` (hidden option `-wasm-o0-reg-coloring-threshold`).
- `[WebAssembly]` (from `swifthost-6.4.2-8`, `f075680ae657`) fork only: register coloring's interference check keeps
  one live interval union per color instead of testing every interval in it; same coloring, linear time.
- `[CodeGen]` (from `swifthost-6.4.2-8`, `6d8c598c2be4`) fork only: a client can stop the legacy pass manager
  part-way (`llvm/IR/CompilationCancellation.h`, a flag per LLVMContext): no further pass, function or loop runs, and
  the long passes stop at complete points. `AsmPrinter` emits nothing after the stop.

## Licenses

LLVM, clang and lld: Apache License 2.0 with LLVM Exceptions (`licenses/llvm-LICENSE.TXT`, `clang-LICENSE.TXT`,
`lld-LICENSE.TXT`), with BLAKE3 (`llvm-Support-BLAKE3-LICENSE`) and the regex code (`llvm-Support-COPYRIGHT.regex`)
in LLVM's Support library. Swift, swift-syntax and swift-experimental-string-processing: Apache License 2.0 with
the Runtime Library Exception (`swift-LICENSE.txt`, `swift-syntax-LICENSE.txt`). swift-cmark: `swift-cmark-COPYING`. Files with their own notices carry them
in their headers at the commits above.

## Release policy

A published `swifthost-*` release is never changed: its assets are never replaced, and a fix ships as the next tag
(`swifthost-6.4.2-2`, ...). Releases are kept while they are among the newest 3 per Swift version or pinned in
`retention-pins.txt`; older unpinned ones are deleted as a whole. If you depend on a release, ask for a pin.

## Rebuilding

On an Apple silicon Mac with Xcode 27.2, CMake and Ninja; about 3 GB for the sources and 1 GB per slice. `VERSION`
is the release id: the slices compile it into the library (`swifthost_version()`), and `package` names the release
after its argument, so pass the same id to both (`VERSION` defaults to `swifthost-6.4.2-3`, whatever you package):

```sh
export VERSION=swifthost-6.4.2-3             # the release id you are building
swifthost/scripts/build.sh sources           # clones the pinned commits into ./src
swifthost/scripts/build.sh native            # host llvm-tblgen and clang-tblgen
swifthost/scripts/build.sh slice iphoneos
swifthost/scripts/build.sh slice iphonesimulator
swifthost/scripts/build.sh slice macosx
swifthost/scripts/build.sh package "$VERSION"
```

`swifthost/test/smoke.c` is the test the releases pass on macOS and in the simulator (see `test/run-smoke.sh`).
`test/run-cancel-stages.sh` cancels a compile in SIL generation, the SIL optimizer, the SIL lowering passes, IR
generation and LLVM optimization (`test/cancel-stages.c`), and `test/sourcekitd/run-cancel.sh` cancels sourcekitd
requests. The simulator runs create a temporary device each, or use the one a test session shares through
`SWIFTHOST_SIM_UDID` (`test/sim-device.sh`, `sim_device_share`); stopping a test script stops its test process and
deletes its device.
