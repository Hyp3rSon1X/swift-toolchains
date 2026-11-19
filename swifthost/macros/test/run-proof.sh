#!/bin/bash
# Proof: expand the fixtures with the wasm module (in-process plugin server = libmacro_host.dylib)
# and with native plugins; diff -dump-macro-expansions output (stderr) and the emitted SIL/IR.
# Baselines: "toolchain" = plugins shipped in the toolchain (Observation/Swift/Testing; Apple's
# FoundationMacros from Xcode) and "samesrc" = the same combined source built natively
# (NATIVE_EXE, executable plugin), which is the only exact baseline for FoundationMacros.
# Usage: run-proof.sh WASM HOSTDYLIB OUTDIR [NATIVE_EXE]
set -o pipefail   # (bash 3.2: no -u, empty arrays)
HERE=$(cd "$(dirname "$0")" && pwd)
WASM=$1; HOST=$2; OUT=$3; NATIVE_EXE=${4:-}
TC=${TC:-$HOME/Library/Developer/Toolchains/swift-6.4.2-hyp3r.9.xctoolchain/usr}
SDK=$(xcrun --show-sdk-path)
XCPLUG=$(xcode-select -p)/Platforms/MacOSX.platform/Developer/usr/lib/swift/host/plugins
mkdir -p "$OUT"
# -frontend does not add the toolchain's implicit plugin paths, so only the listed ones are used.
FE=("$TC/bin/swiftc" -frontend -target arm64-apple-macosx15.0 -sdk "$SDK" -module-name Fix -parse-as-library -I "$TC/lib/swift/macosx/testing")
rc=0
norm() { sed -E 's/@opened\("[0-9A-Fa-f-]+"/@opened("UUID"/g' "$1"; }  # per-run UUIDs in SIL
for f in observation testing swiftmacros foundation; do
  src=$HERE/fixtures/$f.swift
  case $f in observation) wm=ObservationMacros;; testing) wm=TestingMacros;; swiftmacros) wm=SwiftMacros;; foundation) wm=FoundationMacros;; esac
  wasmflags=(-in-process-plugin-server-path "$HOST" -load-resolved-plugin "$WASM##$wm")
  case $f in
    foundation) tcflags=(-plugin-path "$XCPLUG");;
    testing) tcflags=(-plugin-path "$TC/lib/swift/host/plugins/testing");;
    *) tcflags=(-plugin-path "$TC/lib/swift/host/plugins");;
  esac
  for mode in typecheck emit-sil emit-ir; do
    extra=(); [ $mode != typecheck ] && extra=(-O)
    run() { # run LABEL FLAGS...
      local label=$1; shift
      MACRO_HOST_STATS=1 "${FE[@]}" -$mode "${extra[@]}" -dump-macro-expansions "$@" "$src" \
        >"$OUT/$f.$mode.$label.out" 2>"$OUT/$f.$mode.$label.err"; echo $? >"$OUT/$f.$mode.$label.rc"
      grep -v '^macro_host:' "$OUT/$f.$mode.$label.err" >"$OUT/$f.$mode.$label.err.clean"
      norm "$OUT/$f.$mode.$label.out" >"$OUT/$f.$mode.$label.out.norm"
    }
    run wasm "${wasmflags[@]}"
    run toolchain "${tcflags[@]}"
    baselines=toolchain
    if [ -n "$NATIVE_EXE" ]; then run samesrc -load-plugin-executable "$NATIVE_EXE#$wm"; baselines="toolchain samesrc"; fi
    fwd=$(grep -c '^macro_host: forward' "$OUT/$f.$mode.wasm.err")
    for b in $baselines; do
      if cmp -s "$OUT/$f.$mode.wasm.out.norm" "$OUT/$f.$mode.$b.out.norm" && cmp -s "$OUT/$f.$mode.wasm.err.clean" "$OUT/$f.$mode.$b.err.clean"; then
        echo "IDENTICAL $f/$mode vs $b (exit wasm=$(cat "$OUT/$f.$mode.wasm.rc") $b=$(cat "$OUT/$f.$mode.$b.rc"), forwarded=$fwd, out=$(wc -c <"$OUT/$f.$mode.wasm.out")B expansions=$(wc -c <"$OUT/$f.$mode.$b.err.clean")B)"
      else
        echo "DIFFERENT $f/$mode vs $b (forwarded=$fwd): $(diff "$OUT/$f.$mode.$b.err.clean" "$OUT/$f.$mode.wasm.err.clean" | grep -c '^[<>]') differing stderr lines, $(diff "$OUT/$f.$mode.$b.out.norm" "$OUT/$f.$mode.wasm.out.norm" | grep -c '^[<>]') differing stdout lines"; rc=1
      fi
    done
  done
done
exit $rc
