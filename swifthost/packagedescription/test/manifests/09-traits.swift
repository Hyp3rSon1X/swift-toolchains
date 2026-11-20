// swift-tools-version: 6.1
import PackageDescription

let package = Package(
    name: "Traits",
    products: [.library(name: "Traits", targets: ["Traits"])],
    traits: [
        .trait(name: "Logging", description: "Enable logging."),
        .trait(name: "Metrics", description: "Enable metrics.", enabledTraits: ["Logging"]),
        .default(enabledTraits: ["Logging"]),
    ],
    dependencies: [
        .package(url: "https://example.com/a.git", from: "1.0.0", traits: ["Fast", .trait(name: "Safe", condition: .when(traits: ["Logging"]))]),
        .package(url: "https://example.com/b.git", exact: "2.0.0", traits: []),
        .package(url: "https://example.com/c.git", branch: "main", traits: [.defaults]),
        .package(path: "../d", traits: ["X"]),
    ],
    targets: [
        .target(
            name: "Traits",
            dependencies: [.product(name: "A", package: "a", condition: .when(traits: ["Metrics"]))],
            swiftSettings: [.define("LOGGING", .when(traits: ["Logging"]))]
        )
    ]
)
