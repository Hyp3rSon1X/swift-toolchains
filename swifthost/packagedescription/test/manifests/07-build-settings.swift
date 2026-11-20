// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "BuildSettings",
    targets: [
        .target(
            name: "BuildSettings",
            cSettings: [
                .headerSearchPath("include/private"),
                .define("FOO"),
                .define("BAR", to: "2", .when(platforms: [.linux, .android])),
                .unsafeFlags(["-Wall"], .when(configuration: .debug)),
            ],
            cxxSettings: [
                .define("CXX_ONLY", .when(platforms: [.macOS, .iOS], configuration: .release)),
            ],
            swiftSettings: [
                .define("SWIFT_FLAG"),
                .define("DEBUG_ONLY", .when(configuration: .debug)),
                .define("WASI_ONLY", .when(platforms: [.wasi])),
                .enableUpcomingFeature("ExistentialAny"),
                .enableExperimentalFeature("StrictConcurrency", .when(configuration: .release)),
                .unsafeFlags(["-Onone"], .when(platforms: [.macOS], configuration: .debug)),
                .interoperabilityMode(.Cxx, .when(platforms: [.linux])),
            ],
            linkerSettings: [
                .linkedLibrary("z"),
                .linkedLibrary("sqlite3", .when(platforms: [.linux, .macOS])),
                .linkedFramework("CoreData", .when(platforms: [.macOS, .iOS, .tvOS, .watchOS, .visionOS, .macCatalyst])),
                .unsafeFlags(["-Xlinker", "-s"], .when(configuration: .release)),
            ]
        )
    ],
    cLanguageStandard: .gnu11,
    cxxLanguageStandard: .gnucxx20
)
