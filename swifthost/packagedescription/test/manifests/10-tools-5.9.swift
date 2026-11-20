// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "Tools59",
    platforms: [.macOS(.v12)],
    products: [.library(name: "Tools59", targets: ["Tools59"])],
    targets: [
        .target(name: "Tools59", swiftSettings: [.enableUpcomingFeature("BareSlashRegexLiterals")]),
        .testTarget(name: "Tools59Tests", dependencies: ["Tools59"]),
    ],
    swiftLanguageVersions: [.v5]
)
