import Foundation
import PackagePlugin

@main
struct SlowGen: BuildToolPlugin {
    func createBuildCommands(context: PluginContext, target: Target) throws -> [Command] {
        let out = context.pluginWorkDirectoryURL.appending(path: "Generated.swift")
        return [
            .buildCommand(
                displayName: "Generate (holds the build open for 3 s)",
                executable: URL(fileURLWithPath: "/bin/sh"),
                arguments: ["-c", "sleep 3; echo 'let generated = 42' > \"$0\"", out.path],
                inputFiles: [],
                outputFiles: [out]
            )
        ]
    }
}
