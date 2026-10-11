#!/bin/sh
# usr/bin/ld for hyp3r macOS toolchains (installed by assemble-toolchain.sh).
#
# swift.org toolchains ship clang but no `ld`. Their clang looks for `ld` next to itself, then on PATH, and
# otherwise spawns a bare "ld", which fails when PATH is empty: that is the case when Xcode links package
# manifests ("clang: error: unable to execute command: posix_spawn failed: No such file or directory",
# swiftlang/swift#89035). This shim hands the link to the linker that /usr/bin/ld would run: the one of the
# active developer directory (DEVELOPER_DIR if set, as Xcode and xcodebuild do for their child processes,
# otherwise xcode-select), so any Xcode location or name works and Command Line Tools work too.
# TOOLCHAINS is dropped so that xcrun cannot resolve `ld` back into this toolchain.
unset TOOLCHAINS
ld=$(/usr/bin/xcrun --find ld 2>/dev/null)
if [ -z "$ld" ] || [ "$ld" -ef "$0" ]; then
  echo "ld (toolchain shim): cannot find the system linker; install Xcode or the Command Line Tools" >&2
  exit 1
fi
exec "$ld" "$@"
