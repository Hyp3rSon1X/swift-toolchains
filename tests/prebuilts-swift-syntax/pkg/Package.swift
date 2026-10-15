// swift-tools-version: 6.0
import PackageDescription
import CompilerPluginSupport

// A macro target is what makes SwiftPM need swift-syntax's MacroSupport library, i.e. what the prebuilts replace.
let package = Package(
    name: "prebuilts-demo",
    platforms: [.macOS(.v13)],
    dependencies: [
        .package(url: "https://github.com/swiftlang/swift-syntax.git", exact: "604.0.0"),
    ],
    targets: [
        .macro(name: "DemoMacros", dependencies: [
            .product(name: "SwiftSyntaxMacros", package: "swift-syntax"),
            .product(name: "SwiftCompilerPlugin", package: "swift-syntax"),
        ]),
        .target(name: "DemoMacroAPI", dependencies: ["DemoMacros"]),
        .executableTarget(name: "demo", dependencies: ["DemoMacroAPI"]),
    ]
)
