// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "Context",
    targets: [
        .target(
            name: "Context",
            path: Context.packageDirectory + "/Sources",
            swiftSettings: [
                .define("COMMIT_\(Context.gitInformation?.currentCommit ?? "none")"),
                .define("DIRTY", .when(configuration: Context.gitInformation?.hasUncommittedChanges == true ? .debug : .release)),
                .define("TAG_\(Context.gitInformation?.currentTag ?? "none")"),
            ]
        )
    ]
)
