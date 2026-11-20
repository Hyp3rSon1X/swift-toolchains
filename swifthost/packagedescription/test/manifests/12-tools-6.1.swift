// swift-tools-version: 6.1
import PackageDescription

let package = Package(
    name: "Tools61",
    platforms: [.macOS(.v14), .iOS(.v17)],
    products: [.library(name: "Tools61", targets: ["Tools61"])],
    targets: [
        .target(name: "Tools61", swiftSettings: [.swiftLanguageMode(.v6), .enableUpcomingFeature("MemberImportVisibility")]),
        .executableTarget(name: "tool61", dependencies: ["Tools61"]),
    ]
)
