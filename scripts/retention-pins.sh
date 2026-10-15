#!/bin/bash
# Prints the pinned release ids from retention-pins.txt, one per line (comments and blank lines stripped).
# Usage: scripts/retention-pins.sh [file]
set -euo pipefail
f=${1:-retention-pins.txt}
[ -f "$f" ] || exit 0
sed -e 's/#.*//' "$f" | awk 'NF { print $1 }'
