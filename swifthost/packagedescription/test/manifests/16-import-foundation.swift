// swift-tools-version: 5.9
import Foundation
import PackageDescription

// A manifest that imports full Foundation itself: the manifest, not the library, then pulls Foundation (and ICU) in.
let flavour = ProcessInfo.processInfo.arguments.contains("-fileno") ? "driven" : "plain"
let host = URL(string: "https://example.com/pkg.git")?.host ?? "none"
let name = "Foundation-\(flavour)-\(host)-" + "a1b2".uppercased().replacingOccurrences(of: "1", with: "one")

let package = Package(
    name: name,
    targets: [.target(name: "Foundation1")]
)
