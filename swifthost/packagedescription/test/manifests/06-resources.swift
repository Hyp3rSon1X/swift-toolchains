// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "Resources",
    defaultLocalization: "de",
    targets: [
        .target(
            name: "Resources",
            exclude: ["Notes.txt"],
            resources: [
                .process("Assets"),
                .process("Shaders", localization: .base),
                .copy("Data/blob.bin"),
                .embedInCode("Embedded/license.txt"),
            ]
        ),
        .testTarget(name: "ResourcesTests", dependencies: ["Resources"], resources: [.copy("Fixtures")]),
    ]
)
