// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "Dependencies",
    dependencies: [
        .package(url: "https://example.com/a.git", from: "1.2.3"),
        .package(url: "https://example.com/b.git", exact: "2.0.0-beta.1"),
        .package(url: "https://example.com/c.git", "1.0.0"..<"2.0.0"),
        .package(url: "https://example.com/d.git", "1.0.0"..."2.0.0"),
        .package(url: "https://example.com/e.git", .upToNextMinor(from: "0.4.1")),
        .package(url: "https://example.com/f.git", branch: "main"),
        .package(url: "https://example.com/g.git", revision: "0123456789abcdef0123456789abcdef01234567"),
        .package(path: "../local-package"),
        .package(name: "Named", path: "../named-package"),
        .package(url: "https://example.com/h.git", .exact("3.1.4")),
        .package(url: "https://example.com/i.git", .branch("release")),
        .package(url: "https://example.com/j.git", .revision("fedcba9876543210fedcba9876543210fedcba98")),
        .package(url: "https://example.com/k.git", .upToNextMajor(from: "5.0.0")),
    ],
    targets: [
        .target(
            name: "Dependencies",
            dependencies: [
                .product(name: "A", package: "a"),
                .product(name: "C", package: "c", moduleAliases: ["Util": "CUtil"]),
                "Named",
            ]
        )
    ]
)
