// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "ProductsTargets",
    defaultLocalization: "en",
    platforms: [.macOS(.v13), .iOS("16.0"), .tvOS(.v16), .watchOS(.v9), .macCatalyst(.v16), .visionOS(.v1), .driverKit(.v22)],
    pkgConfig: "products-targets",
    providers: [.apt(["libfoo-dev"]), .brew(["foo"])],
    products: [
        .library(name: "Lib", targets: ["Core"]),
        .library(name: "StaticLib", type: .static, targets: ["Core", "Extras"]),
        .library(name: "DynamicLib", type: .dynamic, targets: ["Core"]),
        .executable(name: "tool", targets: ["Tool"]),
        .plugin(name: "GeneratePlugin", targets: ["Generate"]),
    ],
    dependencies: [
        .package(url: "https://example.com/dep.git", from: "1.0.0"),
    ],
    targets: [
        .target(
            name: "Core",
            dependencies: [.target(name: "Extras"), .product(name: "Dep", package: "dep"), .byName(name: "Sys")],
            path: "Sources/Core",
            exclude: ["README.md"],
            sources: ["a.swift", "b.swift"],
            publicHeadersPath: "include",
            plugins: [.plugin(name: "Generate")]
        ),
        .target(name: "Extras"),
        .executableTarget(name: "Tool", dependencies: ["Core"]),
        .testTarget(name: "CoreTests", dependencies: ["Core"], path: "Tests/Core"),
        .systemLibrary(name: "Sys", path: "Sources/Sys", pkgConfig: "sys", providers: [.apt(["libsys-dev"]), .yum(["sys-devel"])]),
        .binaryTarget(name: "Prebuilt", path: "Artifacts/Prebuilt.xcframework"),
        .binaryTarget(
            name: "Remote",
            url: "https://example.com/Remote.zip",
            checksum: "0000000000000000000000000000000000000000000000000000000000000000"
        ),
        .plugin(name: "Generate", capability: .buildTool()),
        .plugin(
            name: "Format",
            capability: .command(
                intent: .sourceCodeFormatting(),
                permissions: [.writeToPackageDirectory(reason: "Formats the sources"), .allowNetworkConnections(scope: .local(ports: [8080]), reason: "Fetches rules")]
            )
        ),
    ],
    swiftLanguageVersions: [.v5, .version("6")],
    cLanguageStandard: .c11,
    cxxLanguageStandard: .cxx17
)
