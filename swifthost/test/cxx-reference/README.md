# cxx-reference

A SwiftPM package with one C++ target (`RefCxxCore`, C++17, a C header as its API) called from a Swift executable
(`RefCxxMain`). It covers what ordinary C++ needs from the SDK's libc++ and libc++abi: global constructors, function-local
statics, `std::map`, `std::vector`, `std::function`, `std::sort`, virtual calls and RTTI (`typeid`), `std::shared_ptr`, `std::mutex`,
`std::thread`, `std::ostringstream` formatting. `std::map::at` and `std::function` are the parts that reference `__cxa_throw` when C++
exceptions are on.

It has no `unsafeFlags`: the threads SDK from `-7` passes `-fno-exceptions` for C++ in its toolset
(`swift-6.4.2-RELEASE_wasm-threads-tailcall`). With an earlier SDK (`-6` and before) the link fails with
`undefined symbol: __cxa_throw`; build with `-Xcxx -fno-exceptions` there.

Build (stock SwiftPM 6.4.2; the SDK bundle unpacked in a directory `<sdks>`, nothing installed):

    swift build --build-system native -v -c release --swift-sdks-path <sdks> \
      --swift-sdk swift-6.4.2-RELEASE_wasm-threads-tailcall

The `-v` output shows `-fno-exceptions` on every C++ compile line. Run the result in a WebAssembly runtime with threads
and tail calls enabled (for example WAMR's `iwasm --stack-size=1048576 --heap-size=16777216 --max-threads=8`):

    iwasm --stack-size=1048576 --heap-size=16777216 --max-threads=8 .build/release/RefCxxMain.wasm

The output must equal `expected-output.txt` exactly, in debug and release.
