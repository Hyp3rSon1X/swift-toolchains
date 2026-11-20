// swift-tools-version: 5.9
import PackageDescription

// Host conditionals are evaluated when the manifest is compiled for its own platform. The wasm build sees
// os(WASI) and arch(wasm32); the host reference sees os(macOS) and arch(arm64). The outputs are expected to differ.
#if os(macOS)
let platformName = "macOS"
#elseif os(Linux)
let platformName = "Linux"
#elseif os(WASI)
let platformName = "WASI"
#else
let platformName = "other"
#endif

#if arch(arm64)
let archName = "arm64"
#elseif arch(x86_64)
let archName = "x86_64"
#elseif arch(wasm32)
let archName = "wasm32"
#else
let archName = "other"
#endif

let package = Package(
    name: "Conditionals-\(platformName)-\(archName)",
    targets: [
        .target(name: "Conditionals", swiftSettings: [.define("BUILT_ON_\(platformName.uppercased())")])
    ]
)
