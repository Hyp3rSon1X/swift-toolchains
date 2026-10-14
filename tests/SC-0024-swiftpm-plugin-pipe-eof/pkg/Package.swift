// swift-tools-version: 6.0
import PackageDescription

// A build-tool plugin whose command keeps the build running for a few seconds after the plugin process has exited, so a
// plugin pipe reader that is never removed has time to spin.
let package = Package(
    name: "PluginPipeEOF",
    targets: [
        .plugin(name: "SlowGen", capability: .buildTool()),
        .executableTarget(name: "demo", plugins: ["SlowGen"]),
    ]
)
