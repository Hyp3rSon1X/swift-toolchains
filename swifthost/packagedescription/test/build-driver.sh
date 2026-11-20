#!/bin/bash
# Builds the small command line driver for the SwiftHostCompiler macOS slice (test/swifthost_drv.c of the tail-call
# SDK recipe): `swifthost_drv <frontend|autolink|ld> <argsfile> <stdout file> <stderr file>`.
# Usage: build-driver.sh <libSwiftHostCompiler.a> <SwiftHostCompiler.h directory> <output>
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
clang -O1 -I "${2:?headers dir}" "$HERE/../../wasm-threads-sdk/test/swifthost_drv.c" "${1:?libSwiftHostCompiler.a}" -o "${3:?output}"
