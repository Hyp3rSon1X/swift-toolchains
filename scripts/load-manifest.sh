#!/bin/bash
# Usage: load-manifest.sh <release-id>  -> appends the manifest's variables to $GITHUB_ENV
set -euo pipefail
f="releases/$1.env"
set -a; . "$f"; set +a
for k in $(grep -oE '^[A-Z][A-Z0-9_]*' "$f"); do printf '%s=%s\n' "$k" "${!k}"; done >> "$GITHUB_ENV"
# The meta job pins BASE_IMAGE and BASE_IMAGE_SLIM to index digests once per run; later jobs pass them in so that the
# whole run uses one base. Later GITHUB_ENV entries win. Without them (mirror.yml, local use) the manifest's own tags stand.
[ -z "${BASE_RESOLVED:-}" ] || printf 'BASE_IMAGE=%s\n' "$BASE_RESOLVED" >> "$GITHUB_ENV"
[ -z "${BASE_SLIM_RESOLVED:-}" ] || printf 'BASE_IMAGE_SLIM=%s\n' "$BASE_SLIM_RESOLVED" >> "$GITHUB_ENV"
