// swift-tools-version: 5.9
import PackageDescription

// Valid registry package identifiers, including the length and character-class edges of the pattern.
let package = Package(
    name: "RegistryValid",
    dependencies: [
        .package(id: "scope.name", from: "1.0.0"),
        .package(id: "a.b", exact: "1.0.0"),
        .package(id: "Scope-1.Name_2-x", "1.0.0"..<"2.0.0"),
        .package(id: "0scope.9name", .upToNextMinor(from: "1.2.0")),
        .package(id: String(repeating: "a", count: 39) + "." + String(repeating: "b", count: 100), from: "0.1.0"),
    ],
    targets: [.target(name: "RegistryValid")]
)
