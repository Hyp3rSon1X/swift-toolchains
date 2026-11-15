// swift-tools-version: 6.4
import PackageDescription

let package = Package(
    name: "Toy",
    targets: [
        .target(name: "Lib", path: "Sources/Lib"),
        .testTarget(
            name: "LibTests",
            dependencies: ["Lib"],
            path: "Tests/LibTests",
            swiftSettings: [
                .unsafeFlags(
                    ["-no-whole-module-optimization", "-enable-batch-mode"],
                    .when(configuration: .release)
                ),
            ]
        ),
    ],
    swiftLanguageModes: [.v6]
)
