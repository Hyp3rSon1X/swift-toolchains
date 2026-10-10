#!/bin/bash
# Usage: load-manifest.sh <release-id>  -> appends the manifest's variables to $GITHUB_ENV
set -euo pipefail
f="releases/$1.env"
set -a; . "$f"; set +a
for k in $(grep -oE '^[A-Z_]+' "$f"); do printf '%s=%s\n' "$k" "${!k}"; done >> "$GITHUB_ENV"
