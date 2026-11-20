// swift-tools-version: 5.9
import PackageDescription

// Each of these is rejected by the registry identifier check; the errors are reported in the JSON.
let package = Package(
    name: "RegistryInvalid",
    dependencies: [
        .package(id: "no-dot", from: "1.0.0"),
        .package(id: "bad id.name", from: "1.0.0"),
        .package(id: "-lead.name", exact: "1.0.0"),
        .package(id: "trail-.name", "1.0.0"..<"2.0.0"),
        .package(id: "scope.name_", from: "1.0.0"),
        .package(id: "a..b", from: "1.0.0"),
        .package(id: String(repeating: "a", count: 40) + ".name", from: "1.0.0"),
        .package(id: "scope." + String(repeating: "b", count: 101), from: "1.0.0"),
        .package(id: "scope.name\nx", from: "1.0.0"),
        .package(id: "scope.näme", from: "1.0.0"),
        .package(id: "", from: "1.0.0"),
    ],
    targets: [.target(name: "RegistryInvalid")]
)
