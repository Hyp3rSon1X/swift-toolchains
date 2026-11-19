# Built-in Swift macro plugins as WebAssembly

One WASI command, `swift-builtin-macros-6.4.2-wasm32-wasip1.wasm`, that serves every macro library the
Swift 6.4.2 toolchain ships, all on a single swift-syntax:

| Module | Source (tag swift-6.4.2-RELEASE) | Types |
|-|-|-|
| ObservationMacros | swiftlang/swift `lib/Macros/Sources/ObservationMacros` | ObservableMacro, ObservationTrackedMacro, ObservationIgnoredMacro |
| SwiftMacros | swiftlang/swift `lib/Macros/Sources/SwiftMacros` | OptionSetMacro, TaskLocalMacro, DebugDescriptionMacro, _DebugDescriptionPropertyMacro, DistributedResolvableMacro, SwiftifyImportMacro, SwiftifyImportProtocolMacro |
| TestingMacros | swiftlang/swift-testing `Sources/TestingMacros` | the 17 types of its TestingMacrosMain.swift |
| FoundationMacros | swiftlang/swift-foundation `Sources/FoundationMacros` | PredicateMacro, ExpressionMacro, BundleMacro |

Each library is its own Swift module, so `String(reflecting:)` yields `ObservationMacros.ObservableMacro`
etc. and swift-syntax's `resolveMacro(moduleName:typeName:)` resolves the same file under any of the four
module names. The libraries' own `@main` files (TestingMacrosMain.swift, FoundationMacros.swift) are excluded;
`Sources/BuiltinMacrosPlugin/BuiltinMacrosPlugin.swift` lists all types instead.

## Plugin ABI (swift-syntax 6.4.2, `CompilerPluginMessageHandler.swift`, `os(WASI)`)

- Exports: `memory`, `_start`, `swift_wasm_macro_v1_pump` (`@_expose(wasm, ...)`, no parameters, no result).
- Imports: `wasi_snapshot_preview1` only (args, environ, fd_close/fdstat_get/prestat*/read/seek/write, path_open,
  proc_exit, random_get).
- `_start` runs `CompilerPlugin.main()`, which only installs the message handler and returns (no read loop).
  The module stays alive; call `swift_wasm_macro_v1_pump` once per host message.
- Each pump reads one frame from fd 0 and writes one frame to fd 1: an 8-byte little-endian UInt64 payload
  length followed by that many bytes of UTF-8 JSON (`HostToPluginMessage` in, `PluginToHostMessage` out).
  Reading blocks, so the host must feed stdin while the pump runs (or pre-fill it); same for large replies.
- Protocol version 8. The module does not implement `loadPluginLibrary` (it throws): the host answers that
  message itself and advertises `features: ["load-plugin-library"]`.
- The host MUST forward the compiler's `getCapability` message to the module (once per instance). The module
  records the host capability from it; without it the module answers with the legacy per-source results and
  formats attached-macro output differently from the native plugins (seen with `@Tag` accessors).
- Macro references in expansion messages carry `moduleName`/`typeName`; map each module name the compiler
  loaded (`-load-resolved-plugin FILE##Module`) to the instance of that file.

## Build

`build.sh [WORKDIR]` (default `./build`): needs `src/{swift-syntax,swift-testing,swift-foundation}`
and a sparse `src/swift` (`lib/Macros`, `LICENSE.txt`), all at swift-6.4.2-RELEASE, the swift.org
`swift-6.4.2-RELEASE_wasm` SDK bundle (`SDKBUNDLE`) and the swift-6.4.2-hyp3r.9 toolchain (`TC`); `WRAP` optionally
prefixes the heavy build command. It symlinks the library sources
into `pkg/Sources`, then runs `swift build --swift-sdks-path ... --swift-sdk swift-6.4.2-RELEASE_wasm -c release
-Xlinker --strip-all`. `package-dist.sh` adds licenses, `macros-provenance.json`, `macros-SHA256SUMS`.
`tools/wasm-inspect.py` lists sections/imports/exports without wabt.

## Proof

`host/macro_host.c` is a test in-process plugin server (`swift_inproc_plugins_handle_message`) running the module
in WAMR 2.4.5 (fast interpreter) with the framing above, one instance per file SHA-256. `host/build-host.sh`
builds `libmacro_host.dylib`; `test/run-proof.sh WASM HOST OUTDIR [NATIVE_EXE]` runs `swiftc -frontend`
(`-typecheck`, `-emit-sil -O`, `-emit-ir -O`, all with `-dump-macro-expansions`) on `test/fixtures/*.swift`
with `-in-process-plugin-server-path HOST -load-resolved-plugin WASM##Module` and with native plugins, then
compares expansions (stderr) and SIL/IR (stdout, `@opened("UUID")` normalized). `test/rss-probe.c` replays a
message log and prints RSS per step.

Baselines: the toolchain's own plugins (Observation, Swift, Testing) and the same sources built natively
(`swift build -c release --product swift-builtin-macros` on macOS, loaded with `-load-plugin-executable`).
FoundationMacros has no matching toolchain plugin (Apple's libFoundationMacros is built with
FOUNDATION_FRAMEWORK and expands to different code), so only the same-source native build is a valid baseline.
The macOS SDK has no FoundationEssentials module, so the foundation fixture cannot compile on either side;
the expansions (the compared output) are identical.
