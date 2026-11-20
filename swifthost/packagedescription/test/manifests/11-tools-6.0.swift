// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "Tools60",
    platforms: [.macOS(.v13), .iOS(.v16)],
    products: [.library(name: "Tools60", targets: ["Tools60"])],
    targets: [
        .target(name: "Tools60", swiftSettings: [.swiftLanguageMode(.v6), .enableUpcomingFeature("InferIsolatedConformances")]),
        .testTarget(name: "Tools60Tests", dependencies: ["Tools60"]),
    ],
    swiftLanguageModes: [.v5, .v6]
)
