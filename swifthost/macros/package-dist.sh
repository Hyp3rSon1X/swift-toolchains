#!/bin/bash
# Stages dist/: licenses, provenance JSON and SHA256SUMS next to the built wasm module.
# Usage: package-dist.sh [WORKDIR]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-/Volumes/MEA-SSD/Hyp3rSuite/SwiftCollection/build/swifthost-macros}
TCROOT=${TCROOT:-$HOME/Library/Developer/Toolchains/swift-6.4.2-hyp3r.9.xctoolchain}
SDKBUNDLE=${SDKBUNDLE:-/Volumes/MEA-SSD/Hyp3rSuite/SwiftCollection/build/swifthost/sdk/swift-6.4.2-RELEASE_wasm.artifactbundle}
SDKTGZ_SHA=83baca83553f8c443cda7b84800912e6e709c3acd2e7ebf277794ca5836c0da5
D=$W/dist; WASM=swift-builtin-macros-6.4.2-wasm32-wasip1.wasm
cp "$W/src/swift-syntax/LICENSE.txt" "$D/LICENSE-swift-syntax.txt"
cp "$W/src/swift-testing/LICENSE.txt" "$D/LICENSE-swift-testing.txt"
cp "$W/src/swift/LICENSE.txt" "$D/LICENSE-swift.txt"
cp "$W/src/swift-foundation/LICENSE.md" "$D/LICENSE-swift-foundation.txt"
SWIFT_VERSION=$("$TCROOT/usr/bin/swift" --version 2>&1 | tr '\n' ' ')
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$TCROOT/Info.plist")
export D WASM W SWIFT_VERSION BUNDLE_ID SDKTGZ_SHA SDKBUNDLE HERE
python3 -I - <<'PY'
import json, os, subprocess, hashlib
D, W = os.environ["D"], os.environ["W"]
def sha(p): return subprocess.check_output(["git", "-C", p, "rev-parse", "HEAD"]).decode().strip()
insp = json.loads(subprocess.check_output(["python3", "-I", os.environ["HERE"] + "/tools/wasm-inspect.py", f"{D}/{os.environ['WASM']}"]))
def types(mod, names): return [{"module": mod, "types": names}]
mods = {
 "ObservationMacros": ["ObservableMacro", "ObservationTrackedMacro", "ObservationIgnoredMacro"],
 "SwiftMacros": ["OptionSetMacro", "TaskLocalMacro", "DebugDescriptionMacro", "_DebugDescriptionPropertyMacro", "DistributedResolvableMacro", "SwiftifyImportMacro", "SwiftifyImportProtocolMacro"],
 "TestingMacros": ["SuiteDeclarationMacro", "TestDeclarationMacro", "ExpectMacro", "RequireMacro", "AmbiguousRequireMacro", "NonOptionalRequireMacro", "ExpectThrowsMacro", "RequireThrowsMacro", "RequireThrowsNeverMacro", "ExitTestExpectMacro", "ExitTestRequireMacro", "ExitTestCapturedValueMacro", "ExitTestBadCapturedValueMacro", "ExitTestIncorrectlyCapturedValueMacro", "TagMacro", "SourceLocationMacro", "PragmaMacro"],
 "FoundationMacros": ["PredicateMacro", "ExpressionMacro", "BundleMacro"],
}
src = lambda n, url: {"repo": url, "tag": "swift-6.4.2-RELEASE", "commit": sha(f"{W}/src/{n}")}
prov = {
 "modules": [{
  "file": os.environ["WASM"], "bytes": os.path.getsize(f"{D}/{os.environ['WASM']}"),
  "sha256": hashlib.sha256(open(f"{D}/{os.environ['WASM']}", "rb").read()).hexdigest(),
  "target": "wasm32-unknown-wasip1", "kind": "WASI command (combined plugin)",
  "macroModules": [{"module": m, "types": t} for m, t in mods.items()],
  "abi": {"startExport": "_start", "pumpExport": "swift_wasm_macro_v1_pump", "framing": "8-byte little-endian length + UTF-8 JSON on stdin (host to module) and stdout (module to host)", "protocolVersion": 8},
  "exports": insp["exports"], "imports": insp["imports"],
  "stripped": True, "customSections": [s["name"] for s in insp["sections"] if s["id"] == 0],
 }],
 "sources": {
  "swift-syntax": src("swift-syntax", "https://github.com/swiftlang/swift-syntax"),
  "swift-testing": src("swift-testing", "https://github.com/swiftlang/swift-testing"),
  "swift-foundation": src("swift-foundation", "https://github.com/swiftlang/swift-foundation"),
  "swift": dict(src("swift", "https://github.com/swiftlang/swift"), paths=["lib/Macros/Sources/ObservationMacros", "lib/Macros/Sources/SwiftMacros"]),
 },
 "compiler": {"swiftVersion": os.environ["SWIFT_VERSION"], "toolchainBundleId": os.environ["BUNDLE_ID"]},
 "wasmSdk": {"id": "swift-6.4.2-RELEASE_wasm", "artifactBundleTarGzSha256": os.environ["SDKTGZ_SHA"], "source": "swift.org"},
 "build": {
  "recipe": "swifthost/macros in swift-toolchains (branch swifthost-6.4.2-2-macros)",
  "command": "swift build --swift-sdks-path <dir with bundle> --swift-sdk swift-6.4.2-RELEASE_wasm -c release --product swift-builtin-macros -Xlinker --strip-all",
  "notes": ["TestingMacrosMain.swift and FoundationMacros.swift (library @main entry points) excluded; replaced by BuiltinMacrosPlugin.swift",
            "TestingMacros built with the swift-testing package settings (ExistentialAny, AccessLevelOnImport, InternalImportsByDefault, MemberImportVisibility, Lifetimes, InferIsolatedConformances, SWT_BUILDING_SWIFT_TESTING_CONTENT); '-allowable-client' flag omitted",
            "FoundationMacros built with the swift-foundation package settings (StrictConcurrency, ImportMacroAliases, InferSendableFromCaptures, MemberImportVisibility, Swift 5 mode, AccessLevelOnImport)"],
 },
 "licenses": {"swift-syntax": "Apache-2.0", "swift-testing": "Apache-2.0", "swift-foundation": "Apache-2.0", "swift": "Apache-2.0 with Runtime Library Exception"},
}
json.dump(prov, open(f"{D}/macros-provenance.json", "w"), indent=2); open(f"{D}/macros-provenance.json", "a").write("\n")
PY
( cd "$D" && rm -f macros-SHA256SUMS && shasum -a 256 "$WASM" LICENSE-*.txt macros-provenance.json > macros-SHA256SUMS && wc -c "$WASM" LICENSE-*.txt macros-provenance.json macros-SHA256SUMS )
