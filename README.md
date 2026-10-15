# swift-toolchains

Official Swift releases with backported fixes, for projects that cannot wait for the next upstream
release. Each release is the unmodified swift.org release plus a small, listed set of patches taken from
upstream fixes (or fixes we intend to upstream), built from public forks under this account.

- Releases: `swift-<version>-hyp3r.<n>` (see `releases/*.env` for the exact fixes and source commits).
- Docker: `ghcr.io/hyp3rson1x/swift:<version>-noble[-slim]` (drop-in for `swift:<version>-noble[-slim]`).
- Mirror on the owner's registry: `registry.hyp3r.dev/swift-toolchains/swift[-slim]:<version>-hyp3r.<n>-noble`
  (same digests as GHCR; newest 3 releases kept).
- Linux toolchain tarballs and overlay tarballs are attached to each GitHub release.

Every fix ships with a reproducer under `tests/` that must fail on the official release and pass on ours,
on x86_64 and aarch64.

Retention: newest 3 releases per Swift version on GitHub, newest 6 image versions on GHCR, newest 3 releases on
the mirror.

## Retention pins

`retention-pins.txt` lists release ids that `retention.yml` and `mirror.yml` never prune (one id per line, comment with
owner, date and reason). A pinned id keeps its GitHub release `swift-<id>`, the GHCR tags `<id>-noble` and `<id>-noble-slim`
(`exclude-tags` of the cleanup action) and the same tags in registry.hyp3r.dev. Pinned ids are not counted against the
limits: the releases-per-line limit and the mirror's newest 3 are computed over the unpinned releases, and the GHCR
`keep-n-tagged` is raised by 2 tags per pin. Remove a pin in a commit as soon as its consumer has moved on.

## SwiftPM swift-syntax prebuilts (full Linux images)

A manifest with `SWIFTPM_PREBUILTS=swift-syntax:604.0.0` and `SWIFTPM_PREBUILTS_SOURCE_TAG=swift-6.4.0-RELEASE` makes the
`image` job bake the official swift.org prebuilts (`download.swift.org/prebuilts/swift-syntax/604.0.0/<tag>-ubuntu_noble_<arch>.json`
and `...-MacroSupport.tar.gz`, byte-identical, archive sha256 checked against the signed manifest) into the full image at
`/opt/hyp3r/swiftpm-prebuilts`, renamed to the compiler tag the built image reports (`scripts/swiftpm-prebuilts.sh`), with
`HYP3R_SWIFTPM_PREBUILTS_URL=file:///opt/hyp3r/swiftpm-prebuilts`. Use:
`swift build --experimental-prebuilts-download-url "$HYP3R_SWIFTPM_PREBUILTS_URL"`. The gate
`tests/prebuilts-swift-syntax/run.sh` builds a macro package against swift-syntax 604.0.0 in the built image and fails the
job unless the prebuilt was used. swift.org has no prebuilts for 6.4.1; there the 6.4.0 files are used (same swiftmodule format).

## Base image digests

The `meta` job resolves `BASE_IMAGE` and `BASE_IMAGE_SLIM` once to `swift:<tag>@sha256:<index digest>` (a manifest that
already pins a digest is used as is); all later jobs get them through `scripts/load-manifest.sh`. The image label
`dev.hyp3r.swift.base` and the release notes carry the digest form.
