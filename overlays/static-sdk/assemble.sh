#!/bin/bash
# Usage: assemble.sh <official-bundle.tar.gz> <built-dir> <out-dir> <release-id> <sdk-version> [source notes...]
#   <built-dir> holds x86_64/{libdispatch.a,libFoundation.a} and aarch64/{...} from build.sh (any subset of arches
#   that is present is swapped; the bundle must still be complete, so CI passes both).
# Produces <out-dir>/swift-<release-id>_static-linux-<sdk-version>.artifactbundle.tar.gz (+ .checksum): the official
# Static Linux SDK with the rebuilt archives swapped in under a distinct artifact id, so it installs next to the
# official one (`swift sdk install` keys on the artifact id in info.json). Work happens in /tmp of the caller, not
# in a bind mount: the bundle contains symlinks.
set -euo pipefail
TARBALL="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; BUILT="$(cd "$2" && pwd)"; mkdir -p "$3"; OUT="$(cd "$3" && pwd)"
RELEASE_ID="$4"; SDKVER="$5"; shift 5
W=$(mktemp -d /tmp/assemble.XXXXXX); trap 'rm -rf "$W"' EXIT
tar xzf "$TARBALL" -C "$W"
old=$(cd "$W" && ls -d *.artifactbundle); old=${old%.artifactbundle}
new="swift-${RELEASE_ID}_static-linux-${SDKVER}"
oldinner="$(cd "$W/$old.artifactbundle" && ls -d */)"; oldinner=${oldinner%/}
[ "$oldinner" = "$old" ] || { echo "::error::unexpected layout: $oldinner vs $old"; exit 1; }
mv "$W/$old.artifactbundle" "$W/$new.artifactbundle"
B="$W/$new.artifactbundle"; mv "$B/$old" "$B/$new"
# info.json: artifact id and variant path. Match the old id literally, nothing else.
sed -i "s/$old/$new/g" "$B/info.json"
grep -q "\"$new\"" "$B/info.json" && ! grep -q "$old" "$B/info.json"

sysroots=("$B/$new"/swift-linux-musl/musl-*.sdk)
[ -d "${sysroots[0]}" ] || { echo "::error::no musl sysroot"; exit 1; }
M="$B/MANIFEST.txt"
{
  echo "Swift Static Linux SDK ${SDKVER} for Swift ${RELEASE_ID%%-*}, patched (${RELEASE_ID})"
  echo "artifact id: $new"
  echo "base: official $old.artifactbundle.tar.gz, sha256 $(sha256sum "$TARBALL" | cut -d' ' -f1)"
  echo "replaced archives (usr/lib/swift_static/linux-static/), everything else is byte-identical to the base:"
} > "$M"
for arch in x86_64 aarch64; do
  [ -d "$BUILT/$arch" ] || { echo "::error::no rebuilt archives for $arch"; exit 1; }
  dest="${sysroots[0]}/$arch/usr/lib/swift_static/linux-static"
  for a in libdispatch.a libFoundation.a; do
    before=$(sha256sum "$dest/$a" | cut -d' ' -f1)
    install -m 0644 "$BUILT/$arch/$a" "$dest/$a"
    echo "  $arch/$a: $before (official) -> $(sha256sum "$dest/$a" | cut -d' ' -f1)" >> "$M"
  done
done
{ echo "sources:"; for s in "$@"; do echo "  $s"; done; echo "build: overlays/static-sdk/build.sh (swift-docker static-linux recipe); global symbols checked identical to the base"; } >> "$M"

name="$new.artifactbundle.tar.gz"
# Same member order as the official tarball: info.json and the sysroot are under the single top-level directory.
(cd "$W" && tar czf "$OUT/$name" "$new.artifactbundle")
sum=$(swift package compute-checksum "$OUT/$name" 2>/dev/null || sha256sum "$OUT/$name" | cut -d' ' -f1)
echo "$sum" > "$OUT/$name.checksum"
cp "$M" "$OUT/MANIFEST-static-sdk.txt"
cat "$M"; echo "tarball: $name"; echo "checksum: $sum"; ls -la "$OUT/$name"
