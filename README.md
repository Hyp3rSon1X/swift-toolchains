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
