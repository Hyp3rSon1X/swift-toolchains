#!/bin/bash
# Builds and installs SwiftPM the way the official Swift Linux toolchain does, inside the official
# swift:<SWIFT_VERSION>-noble image, then compares the result with the image's own SwiftPM.
# Build recipe from SwixForge's swix/6.4.0 branch of the SwiftPM fork.
#
# Usage: build.sh <out dir>        (after workspace.sh)
# Results in <out dir>: usr/bin/swift-package (the overlay: the only file that is replaced), installed-files.txt,
# parity.txt, MANIFEST-swiftpm.txt; fails if the parity checks fail.
# Env: SWIFTPM_REF (recorded in the manifest)
#
# Why only usr/bin/swift-package: the toolchain ships SwiftPM as one executable; swift-build, swift-test, swift-run,
# swift-sdk, ... are symlinks to it, and the fix (SPMBuildCore) is linked statically into it. lib/swift/pm/ManifestAPI
# and PluginAPI are untouched by it. usr/bin/sourcekit-lsp links the same modules statically and is not rebuilt.
#
# Parity source: swiftlang/swift at swift-<SWIFT_VERSION>-RELEASE.
# - utils/build-presets.ini [preset: buildbot_linux]: `release`, `swiftpm`, `install-swiftpm`, install-prefix=/usr.
# - utils/swift_build_support/swift_build_support/products/swiftpm.py: build-script runs
#   `Utilities/bootstrap build --release ...` and then `Utilities/bootstrap install --release ... --prefix <destdir>/usr`;
#   the installed swift-package comes from the install run, which builds with SWIFTCI_USE_LOCAL_DEPS=1,
#   SWIFTCI_INSTALL_RPATH_OS=linux and --disable-local-rpath (Utilities/bootstrap get_swiftpm_env_cmd/get_swiftpm_flags).
# - Difference: build-script first builds a CMake bootstrap SwiftPM (swift-bootstrap) and drives the final build with it;
#   we pass --skip-cmake-bootstrap, so the image's own swift-build (same SwiftPM release, same Swift Build
#   backend) drives it.
set -euo pipefail
out=$(realpath -m "$1")
workspace=/home/build-user
case "$(uname -m)" in
    x86_64) arch=x86_64 ;;
    aarch64) arch=aarch64 ;;
esac
build_dir=$workspace/build/buildbot_linux/swiftpm-linux-$arch
destdir=$workspace/swift-nightly-install
mkdir -p "$out/usr/bin"
rm -rf "$destdir"
# build-script installs other products into the same prefix first; Utilities/bootstrap copies
# swift-package into usr/bin without creating it.
mkdir -p "$destdir/usr/bin"

(cd "$workspace/swiftpm" && python3 Utilities/bootstrap install \
    --release \
    --skip-cmake-bootstrap \
    --swiftc-path /usr/bin/swiftc \
    --clang-path /usr/bin/clang \
    --build-dir "$build_dir" \
    --prefix "$destdir/usr")

install -m 0755 "$destdir/usr/bin/swift-package" "$out/usr/bin/swift-package"
(cd "$destdir" && find . \( -type f -o -type l \) | sort) > "$out/installed-files.txt"

# Parity with the image's SwiftPM.
{
    rc=0
    dyn() { readelf -d --wide "$1" | grep -E '\((NEEDED|RUNPATH|RPATH)\)' | awk '{print $2, $NF}'; }
    if diff <(dyn /usr/bin/swift-package) <(dyn "$out/usr/bin/swift-package") > /dev/null; then
        echo "same   NEEDED+RUNPATH ($(dyn /usr/bin/swift-package | wc -l) entries)"
        dyn "$out/usr/bin/swift-package" | sed 's/^/         /'
    else
        echo "DIFFER NEEDED+RUNPATH"; diff <(dyn /usr/bin/swift-package) <(dyn "$out/usr/bin/swift-package"); rc=1
    fi
    # The installed binary finds the Swift runtime through RUNPATH $ORIGIN/../lib/swift/linux,
    # so run it from a prefix whose lib is the image's own.
    probe=$workspace/probe-prefix
    rm -rf "$probe"; mkdir -p "$probe/bin"; ln -s /usr/lib "$probe/lib"
    cp "$out/usr/bin/swift-package" "$probe/bin/swift-package"
    stock_version=$(/usr/bin/swift-package --version)
    built_version=$("$probe/bin/swift-package" --version)
    rm -rf "$probe"
    if [[ "$stock_version" == "$built_version" ]]; then
        echo "same   --version: $built_version"
    else
        echo "DIFFER --version: stock '$stock_version', built '$built_version'"; rc=1
    fi
    # Every installed file must exist in the image, with the same type (file or symlink target).
    missing=0
    while read -r f; do
        p=/${f#./}
        if [[ -L "$destdir/$f" ]]; then
            [[ -L "$p" && "$(readlink "$p")" == "$(readlink "$destdir/$f")" ]] || { echo "not in image as the same symlink: $p"; missing=1; }
        else
            [[ -f "$p" && ! -L "$p" ]] || { echo "not in image: $p"; missing=1; }
        fi
    done < "$out/installed-files.txt"
    if [[ $missing == 0 ]]; then echo "same   installed file set ($(wc -l < "$out/installed-files.txt") files, all present in the image)"; else rc=1; fi
    file "$out/usr/bin/swift-package" | sed 's/^[^:]*: //'
    exit $rc
} | tee "$out/parity.txt"

{
    echo "component: swiftpm"
    echo "source: ${SWIFTPM_REF:-unknown}"
    echo "arch: $(uname -m)"
    echo "replaces: usr/bin/swift-package only (swift-build, swift-test, swift-run, ... are symlinks to it)"
    echo "parity (NEEDED/RUNPATH, --version, installed file set): see parity.txt"
    (cd "$out" && sha256sum usr/bin/swift-package)
} > "$out/MANIFEST-swiftpm.txt"
cat "$out/MANIFEST-swiftpm.txt"
