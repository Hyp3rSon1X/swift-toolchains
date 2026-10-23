#!/bin/bash
# Runs inside a throwaway container of the official image. Usage: run.sh <overlay-dir>
# SC-0032: in actor- or global-actor-isolated code an optional-chained mutation (dict[k]?.x = await f(),
# ?.x += await, ?.append(await f())) begins the access before the right-hand side and holds it across the
# suspension: concurrent actor tasks lose updates, with class payloads use-after-free/SIGSEGV. Stock compilers
# say nothing; the patched frontend warns (group ActorIsolatedOptionalChainSuspension) at exactly the affected
# sites and nowhere else (hoisted await, `!` chain, await in a separate statement), and -Werror <group> makes it
# an error. Stock is informational. STOCK_ONLY=1 skips the swap.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; overlay="${1:-}"
FE=/usr/bin/swift-frontend
GROUP=ActorIsolatedOptionalChainSuspension
EXPECTED="calls.swift:10 calls.swift:11 calls.swift:12 forms.swift:13 forms.swift:7 lost_update.swift:7 lost_update.swift:8"

sites() { # prints sorted file:line of every SC-0032 warning
  local f
  for f in lost_update.swift forms.swift calls.swift; do
    (cd "$here" && timeout 120 swiftc -swift-version 6 -parse-as-library -typecheck "$f" 2>&1) \
      | grep -E "^[^ ]+:[0-9]+:[0-9]+: warning: .*\[#$GROUP\]" | sed -E 's/^([^:]+):([0-9]+):.*/\1:\2/' | sed 's|.*/||'
  done | sort -u | tr '\n' ' ' | sed 's/ $//'
}

stock=$(sites)
echo "stock sites: ${stock:-none}"
[ -z "$stock" ] || echo "::warning::the stock compiler already emits $GROUP; check whether upstream shipped it"
if [ "${STOCK_ONLY:-0}" = 1 ]; then echo "SUMMARY stock_sites=${stock:-none} patched=skipped"; exit 0; fi

echo "stock frontend:   $("$FE" -version 2>&1 | head -1)"
cp "$FE" /tmp/swift-frontend.stock
install -m 0755 "$overlay/usr/bin/swift-frontend" "$FE"
echo "patched frontend: $("$FE" -version 2>&1 | head -1)"
echo "patched sha256: $(sha256sum "$FE" | cut -c1-16)"

patched=$(sites)
echo "patched sites:  $patched"
echo "expected sites: $EXPECTED"
ok=1; [ "$patched" = "$EXPECTED" ] || { echo "FAIL: warning sites differ"; ok=0; }
if (cd "$here" && timeout 120 swiftc -swift-version 6 -parse-as-library -typecheck -Werror "$GROUP" forms.swift >/dev/null 2>&1); then
  echo "FAIL: -Werror $GROUP did not turn the warning into an error"; ok=0
else echo "patched -Werror $GROUP: error as expected"; fi
echo "SUMMARY stock_sites=${stock:-none} patched_match=$ok"
[ "$ok" = 1 ]
