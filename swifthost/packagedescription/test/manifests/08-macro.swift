// swift-tools-version: 5.9
import CompilerPluginSupport
import PackageDescription

let package = Package(
    name: "MacroPackage",
    platforms: [.macOS(.v10_15), .iOS(.v13)],
    products: [
        .library(name: "MacroClient", targets: ["MacroClient"]),
        .executable(name: "MacroClientDemo", targets: ["MacroDemo"]),
    ],
    dependencies: [
        .package(url: "https://github.com/swiftlang/swift-syntax.git", from: "510.0.0"),
    ],
    targets: [
        .macro(
            name: "MacroImpl",
            dependencies: [
                .product(name: "SwiftSyntaxMacros", package: "swift-syntax"),
                .product(name: "SwiftCompilerPlugin", package: "swift-syntax"),
            ]
        ),
        .target(name: "MacroClient", dependencies: ["MacroImpl"]),
        .executableTarget(name: "MacroDemo", dependencies: ["MacroClient"]),
        .testTarget(name: "MacroTests", dependencies: ["MacroImpl"]),
    ]
)
