# SwiftHostCompiler

The Swift 6.4.2 compiler, clang and wasm-ld as a static library for iOS, the iOS simulator and macOS (arm64,
deployment target 27.0). An app links it and compiles Swift source to WebAssembly (wasm32-unknown-wasip1) in its
own process: no subprocesses, no JIT, and no dynamic loading unless the app names a plugin server library. The output
runs in any WASI preview 1 runtime, for example WAMR in interpreter mode.

It compiles against the official swift.org Swift SDK for WebAssembly, `swift-6.4.2-RELEASE_wasm`
(artifactbundle SHA-256 `83baca83553f8c443cda7b84800912e6e709c3acd2e7ebf277794ca5836c0da5`), which the app ships
or downloads itself. Programs can use the standard library, Foundation (FoundationEssentials,
FoundationInternationalization), Observation's runtime, Synchronization, RegexBuilder and the other modules in that
SDK. Macros work through an in-process plugin server that the app provides (see Macros).

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
hidden except the six `swifthost_*` functions. LLVM, clang or Swift symbols in the app or in other libraries
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
   link with `swifthost_wasm_ld`. Each takes an argv and two file descriptors (output, diagnostics) and returns the
   tool's exit status.
3. Pass `-module-cache-path` to a writable directory that survives between builds (for example in Caches).
4. If a call returns `SWIFTHOST_EPOISONED` (125), the compiler crashed in this process (in this call or an earlier
   one): tell the user to restart the app before compiling again.
5. To stop a compile, call `swifthost_request_cancel()` from another thread; the frontend call returns
   `SWIFTHOST_ECANCELED` (124) and the library stays usable.

## Macros

The compiler includes swift-syntax and expands macros like `swiftc` does with an in-process plugin server: it sends
each request (`getCapability`, `loadPluginLibrary`, `expandFreestandingMacro`, `expandAttachedMacro`) as a JSON
message to a function and gets the reply back from it. The library contains no macro implementations; the app runs
them (for example as WebAssembly plugin modules in its own runtime) and answers the messages. Two ways:

- `swifthost_set_plugin_message_handler(handler)`, once, before the first compile: the app's own function receives
  every message (same contract as `swift_inproc_plugins_handle_message`; the reply is a `malloc`ed buffer that the
  compiler frees). Frontend calls still pass `-in-process-plugin-server-path <any path>`, which selects the route;
  the path is never opened.
- `-in-process-plugin-server-path <library>`: a dynamic library that exports `swift_inproc_plugins_handle_message`,
  loaded by the compiler (macOS; not for App Store iOS apps).

Each macro module is named with `-load-resolved-plugin <file>##<Module>`; the server gets the file's resolved path
in `loadPluginLibrary`. See `JOBS.txt` for the arguments and message formats. A malformed or hostile reply is an
error diagnostic for that macro (return value 1); a reply nested so deeply that the JSON parser overflows the
calling thread's stack is recovered like a compiler crash. Executable plugins (`-load-plugin-executable`,
`-external-plugin-path`) are compiled out: they fail with "executable plugins are not supported in this build",
and no process is started.

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
type check within 2-8 ms, among top-level statements as well as among function bodies.

In the iOS 27.2 simulator on the same Mac, in two runs while other builds were using the Mac, every step was 5-100x
slower (hello world 1.3-9 s for the first build and 0.03-0.6 s later, Foundation frontend 0.8-8.8 s, wasm-ld 0.6-7.4 s,
cancel within 0.08-1.4 s; peak memory about the same). We did not find out why; processes started with `simctl spawn`
may run at a low scheduling priority. Measure on a device.

## Trimming the SDK

The jobs in `JOBS.txt` read only part of the `wasm32-unknown-wasip1` directory (338 MiB, 3,324 files). With these
removed, the smoke test still passes (164 MiB, 1,125 files left):

- `swift.xctoolchain/usr/lib/swift` (148 MiB): the resource directory for dynamic linking; the jobs use
  `swift_static` (`-use-static-resource-dir`).
- every `*.swiftinterface`: the binary `.swiftmodule`s load directly, and no module is rebuilt into the module cache.
- every `*.swiftdoc` (documentation comments; opened when present, not needed).
- `WASI.sdk/include/c++` (libc++ headers; only needed for C++ interoperability, which the jobs do not enable).

`test/sdk-access-smoke.txt` lists every SDK file that the smoke test's programs opened or checked for (recorded with
`test/fsaccess.c`). Keep files that are only checked for (`probe`), such as headers named in module maps: clang
looks them up while building a module. A program that imports a module the smoke test does not use (RegexBuilder,
Synchronization, ...) needs that module's `.swiftmodule` directory and its `lib*.a` as well.

## Limits

- One call at a time per process (calls are serialized internally).
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

## Source and patches

Built from public forks, on top of Swift 6.4.2:

| Repository | Branch | Commit |
|-|-|-|
| [Hyp3rSon1X/swift](https://github.com/Hyp3rSon1X/swift) | `hyp3r/ios-host/6.4.2` | `320cb47426b5cfeb59909deb4b3eb0453a9bdc43` |
| [Hyp3rSon1X/llvm-project](https://github.com/Hyp3rSon1X/llvm-project) | `hyp3r/ios-host/6.4.2` | `af10e9a7844d303de78404f574a453912edcee77` |
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
after its argument, so pass the same id to both (`VERSION` defaults to `swifthost-6.4.2-2`, whatever you package):

```sh
export VERSION=swifthost-6.4.2-2             # the release id you are building
swifthost/scripts/build.sh sources           # clones the pinned commits into ./src
swifthost/scripts/build.sh native            # host llvm-tblgen and clang-tblgen
swifthost/scripts/build.sh slice iphoneos
swifthost/scripts/build.sh slice iphonesimulator
swifthost/scripts/build.sh slice macosx
swifthost/scripts/build.sh package "$VERSION"
```

`swifthost/test/smoke.c` is the test the releases pass on macOS and in the simulator (see `test/run-smoke.sh`).
