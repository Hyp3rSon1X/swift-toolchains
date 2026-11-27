// swift-tools-version:6.0
// Reference package for the swifthost_clang C++ milestone: one C++ target with a C API, called from Swift.
import PackageDescription

let package = Package(
  name: "RefCxx",
  targets: [
    // No -fno-exceptions here: the threads SDK from -7 passes it for C++ in its toolset. The SDK's libc++abi has no
    // exception runtime, so with exceptions on (clang's default) std::map::at, std::function and friends fail to link.
    // With an earlier SDK, build with `-Xcxx -fno-exceptions`. (unsafeFlags would make this unusable as a dependency.)
    .target(name: "RefCxxCore", path: "Sources/RefCxxCore", publicHeadersPath: "include"),
    .executableTarget(name: "RefCxxMain", dependencies: ["RefCxxCore"], path: "Sources/RefCxxMain"),
  ],
  cxxLanguageStandard: .cxx17
)
