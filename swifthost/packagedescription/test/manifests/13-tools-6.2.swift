// swift-tools-version: 6.2
import PackageDescription

let package = Package(
    name: "Tools62",
    platforms: [.macOS(.v15), .iOS(.v18)],
    products: [.library(name: "Tools62", targets: ["Tools62"])],
    targets: [
        .target(
            name: "Tools62",
            swiftSettings: [.defaultIsolation(MainActor.self), .strictMemorySafety(), .enableUpcomingFeature("NonisolatedNonsendingByDefault"), .treatAllWarnings(as: .error), .treatWarning("DeprecatedDeclaration", as: .warning)]
        ),
        .testTarget(name: "Tools62Tests", dependencies: ["Tools62"]),
    ]
)
