// swift-tools-version: 6.2
// Combined wasm macro plugin: every built-in macro library as its own Swift module (so
// String(reflecting:) keeps the original "Module.Type" names) behind one @main plugin that
// shares a single swift-syntax. build.sh symlinks the library sources into Sources/ first.
import PackageDescription

let syntax = Context.environment["SWIFT_SYNTAX_PATH"] ?? "../src/swift-syntax"

let syntaxDeps: [Target.Dependency] = [
  .product(name: "SwiftDiagnostics", package: "swift-syntax"),
  .product(name: "SwiftOperators", package: "swift-syntax"),
  .product(name: "SwiftParser", package: "swift-syntax"),
  .product(name: "SwiftParserDiagnostics", package: "swift-syntax"),
  .product(name: "SwiftSyntax", package: "swift-syntax"),
  .product(name: "SwiftSyntaxBuilder", package: "swift-syntax"),
  .product(name: "SwiftSyntaxMacroExpansion", package: "swift-syntax"),
  .product(name: "SwiftSyntaxMacros", package: "swift-syntax"),
]

let package = Package(
  name: "swift-builtin-macros",
  products: [.executable(name: "swift-builtin-macros", targets: ["BuiltinMacrosPlugin"])],
  dependencies: [.package(path: syntax)],
  targets: [
    .target(
      name: "ObservationMacros", dependencies: syntaxDeps,
      exclude: ["CMakeLists.txt"]),
    .target(
      name: "SwiftMacros", dependencies: syntaxDeps,
      exclude: ["CMakeLists.txt"]),
    .target(
      name: "TestingMacros", dependencies: syntaxDeps,
      // TestingMacrosMain.swift is the library's own @main; the combined plugin replaces it.
      exclude: ["CMakeLists.txt", "TestingMacrosMain.swift"],
      swiftSettings: [
        .enableUpcomingFeature("ExistentialAny"),
        .enableExperimentalFeature("AccessLevelOnImport"),
        .enableUpcomingFeature("InternalImportsByDefault"),
        .enableUpcomingFeature("MemberImportVisibility"),
        .enableExperimentalFeature("Lifetimes"),
        .enableUpcomingFeature("InferIsolatedConformances"),
        .define("SWT_BUILDING_SWIFT_TESTING_CONTENT"),
        .unsafeFlags(["-Xfrontend", "-disable-implicit-string-processing-module-import"]),
      ]),
    .target(
      name: "FoundationMacros", dependencies: syntaxDeps,
      // FoundationMacros.swift is the library's own @main.
      exclude: ["CMakeLists.txt", "FoundationMacros.swift"],
      swiftSettings: [
        .enableExperimentalFeature("AccessLevelOnImport"),
        .enableExperimentalFeature("StrictConcurrency"),
        .enableExperimentalFeature("ImportMacroAliases"),
        .enableUpcomingFeature("InferSendableFromCaptures"),
        .enableUpcomingFeature("MemberImportVisibility"),
        .swiftLanguageMode(.v5),
      ]),
    .executableTarget(
      name: "BuiltinMacrosPlugin",
      dependencies: [
        "ObservationMacros", "SwiftMacros", "TestingMacros", "FoundationMacros",
        .product(name: "SwiftCompilerPlugin", package: "swift-syntax"),
        .product(name: "SwiftSyntaxMacros", package: "swift-syntax"),
      ]),
  ])
