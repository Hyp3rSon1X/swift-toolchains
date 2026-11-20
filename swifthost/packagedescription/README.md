# SwiftPM PackageDescription for WebAssembly (threads, tail calls)

`PackageDescription` and `CompilerPluginSupport` from SwiftPM 6.4.2, built as static libraries for
`wasm32-unknown-wasip1-threads` with WebAssembly tail calls. A `Package.swift` manifest compiled against them is a
WASI command: run it in a WebAssembly runtime with `-fileno 1` and it writes the manifest JSON to standard output,
exactly as SwiftPM reads it from a natively built manifest.

They match the `swift-6.4.2-RELEASE_wasm-threads-tailcall` Swift SDK (release `swifthost-sdk-wasip1-threads-6.4.2-2`).
Like everything compiled for that SDK, they are built with `-Xcc -mtail-call`; a manifest must be compiled with the same
flag, which the SDK's toolset sets.

## Variants

|  | `essentials` | `foundation` |
|-|-|-|
| Sources | SwiftPM `swift-6.4.2-RELEASE` plus `swiftpm-wasi-foundation-essentials.patch` | SwiftPM `swift-6.4.2-RELEASE`, unpatched |
| Imports | `FoundationEssentials` and `WASILibc` (implementation-only) | `Foundation` (implementation-only) |
| ICU, FoundationInternationalization, CoreFoundation | not linked | linked |
| `Package.swift` that imports only `PackageDescription` | 18 MB module, 94 MiB peak RSS in WAMR | 61 MB module, 193 MiB peak RSS |

Use `essentials` unless a manifest imports `Foundation` itself and you want the libraries to share one Foundation; then
either variant works, and the manifest pulls Foundation in on its own account.

The patch (5 files, 35 insertions, 1 deletion) is guarded with `#if os(WASI) && canImport(FoundationEssentials)`; every
other platform keeps importing Foundation:
- `Foundation` becomes `FoundationEssentials` in `PackageDescription.swift`, `PackageDependency.swift`, `Target.swift`,
  `PackageDescriptionSerialization.swift` and `ContextModel.swift` (which SwiftPM shares with `PackageLoading` by a
  symbolic link), keeping `@_implementationOnly` and `USE_IMPL_ONLY_IMPORTS` as they are;
- `PackageDescription.swift` imports `WASILibc` for `fdopen`, `fputs`, `fclose` and `atexit`, which Foundation used to
  re-export;
- the registry identifier check in `PackageDependency.swift` used `String.range(of:options:.regularExpression)` (ICU).
  It builds a standard library `Regex` from the same pattern. The error text is unchanged.

## Contents of each zip

| Path | |
|-|-|
| `PackageDescription.swiftmodule/`, `CompilerPluginSupport.swiftmodule/` | `wasm32-unknown-wasip1-threads.{swiftmodule,swiftdoc,swiftinterface,private.swiftinterface}` |
| `lib/libPackageDescription.a`, `lib/libCompilerPluginSupport.a` | the static libraries |
| `lib/link-libs.txt` | link libraries the archives need (see below) |
| `LICENSE.txt`, `NOTICE.txt` | from SwiftPM |
| `provenance.json` | sources, commits, compiler, SDK, flags, patch checksum |
| `swiftpm-wasi-foundation-essentials.patch` | `essentials` only |

`SHA256SUMS` lists the zips and the patch.

## Using them

Compile a manifest with the SDK's flags and `-I <unpacked zip>`; link `-L <unpacked zip>/lib -lCompilerPluginSupport
-lPackageDescription` and the libraries in `lib/link-libs.txt`:

```
swiftc -target wasm32-unknown-wasip1-threads -sdk <sdk>/WASI.sdk -resource-dir <sdk>/swift.xctoolchain/usr/lib/swift_static \
  -static-stdlib -Xclang-linker -resource-dir -Xclang-linker <sdk>/swift.xctoolchain/usr/lib/swift_static/clang \
  -Xcc -matomics -Xcc -mbulk-memory -Xcc -mthread-model -Xcc posix -Xcc -pthread -Xcc -ftls-model=local-exec -Xcc -mtail-call \
  -swift-version <5 below tools version 6.0, else 6> -package-description-version <the manifest's tools version> \
  -I <zip> -L <zip>/lib -lCompilerPluginSupport -lPackageDescription $(cat <zip>/lib/link-libs.txt) \
  Package.swift -o manifest.wasm \
  -Xlinker --import-memory -Xlinker --export-memory -Xlinker --shared-memory -Xlinker --max-memory=1073741824
```

- The libraries import Foundation implementation-only, so the module files do not carry its link libraries.
  `link-libs.txt` is what `swift-autolink-extract` reports for the two objects (`-lFoundationEssentials` and its
  dependencies, or `-lFoundation`, `-l_FoundationICU` and so on). With SwiftHostCompiler, append those lines to the
  `swift-autolink-extract` output of the manifest's object file before the `wasm-ld` call.
- Run it in a runtime with tail calls and wasi-threads (for example WAMR built with `WAMR_BUILD_TAIL_CALL=1` and
  `WAMR_BUILD_LIB_WASI_THREADS=1`): `iwasm --stack-size=1048576 --heap-size=16777216 manifest.wasm -context <json> -fileno 1`.
  `-fileno <fd>` names the descriptor PackageDescription writes the JSON to. `-context` carries the package directory and
  git information for manifests that use `Context`; it is optional otherwise.
- The `.swiftmodule` files are tied to the compiler that built them (Swift 6.4.2 with the tail-call changes of the SDK
  recipe). The `.swiftinterface` files are there for reference and for toolchains that can build them; compiling a
  manifest from them needs `-Xcc -mtail-call` and the same SDK.
- Conditional compilation in a manifest sees the platform the manifest is compiled for: `#if os(WASI)` and
  `arch(wasm32)` are true, `os(macOS)` and `os(Linux)` are false. A manifest that branches on the host platform produces
  different JSON than when SwiftPM builds it natively. This is inherent, not a defect of the libraries.

## Building

```
SDK_BUNDLE=<unpacked swift-6.4.2-RELEASE_wasm-threads-tailcall.artifactbundle> \
SWIFTC=<swiftc that built the SDK> TOOLS=<directory with wasm-ld and llvm-ar> OUT=<empty directory> ./build.sh
OUT=<same> SDK_ZIP=<the SDK zip> DIST=<release directory> ./package.sh
```

`build.sh` clones SwiftPM (`SWIFTPM_REPO`, default upstream) at the pinned commit
`8054c59f07a9c8cc8e67b3ec57870710c7154854`, applies the patch to a second copy, and compiles both variants with SwiftPM's
own flags (`-swift-version 5 -enable-library-evolution -package-description-version 999.0`, `-D USE_IMPL_ONLY_IMPORTS`
for PackageDescription, module interfaces emitted) plus `-O`, `-wmo`, `-parse-as-library` and the SDK flags. It also
sets `-enable-upcoming-feature ConciseMagicFile`, which turns `#file` in two `fatalError` messages from the absolute
build path into `PackageDescription/PackageDependency.swift`, so the archives hold no local paths.
`package.sh` writes one zip per variant with fixed modification times and sorted entries, so repackaging the same build
gives the same zips. The compiler output itself is not reproducible: each object file holds a 16-byte value that
changes from compile to compile, so a rebuild gives different archive checksums.

## Tests

`test/` holds 16 manifests (`test/manifests`) and the scripts that check the libraries against the host toolchain:

| Manifest | Covers |
|-|-|
| `01-minimal` | smallest package |
| `02-dependencies` | `from`, `exact`, ranges, `upToNextMinor`/`upToNextMajor`, `branch`, `revision`, `path`, and the deprecated spellings |
| `03-registry-valid`, `04-registry-invalid` | registry identifiers, valid and invalid, with the length and character edges of the pattern; the error text in the JSON must match |
| `05-products-targets` | every product and target kind, platforms, providers, plugins and their capabilities, language standards |
| `06-resources` | `process`, `copy`, `embedInCode`, localization |
| `07-build-settings` | C, C++, Swift and linker settings with `.when(platforms:configuration:)` |
| `08-macro` | a macro target through `CompilerPluginSupport` |
| `09-traits` | traits, default traits, traits on dependencies (tools version 6.1) |
| `10-tools-5.9` to `13-tools-6.2` | one per tools version |
| `14-conditionals` | `#if os(...)` and `#if arch(...)`; expected to differ from the host |
| `15-context` | `Context.packageDirectory` and `Context.gitInformation` |
| `16-import-foundation` | a manifest that imports `Foundation` itself |

Scripts, each documented in its header:
- `host-ref.sh <toolchain usr dir> <out>` compiles every manifest with a host toolchain against its own
  PackageDescription (flags as in SwiftPM's `ManifestLoader.swift`) and runs it natively with `-fileno 1`;
- `smoke-swiftc.sh` and `smoke-swifthost.sh` compile, link and run every manifest in WAMR with `swiftc` or with the
  SwiftHostCompiler macOS slice, and compare the JSON with the host reference byte for byte; they record wasm-ld
  warnings, signature mismatches, interface rebuilds, wall time and peak RSS;
- `build-driver.sh` builds the small command line driver for SwiftHostCompiler that `smoke-swifthost.sh` uses;
- `check-icu.sh` counts ICU, FoundationInternationalization and CoreFoundation symbols in a linked module;
- `run-all.sh` runs all of it and prints the summary (`summarize.py`).

Results for this release are in the release notes.
