#!/bin/bash
# macOS smoke test of the in-process sourcekitd with skd-probe: for each Swift probe source of the
# wasm-threads SDK, diagnostics must show 0 errors, completion at a defined point must return the
# expected member and cursor info on a known declaration must return its name and USR. Four
# deliberately broken copies (written to <work-dir>/broken) must give the expected error.
#
#   run-smoke.sh <skd-probe binary> <sdk-target-dir> <work-dir> <wasm-threads-sdk dir>
#
# <skd-probe binary> is built for $SLICE (macosx by default; SLICE=iphonesimulator runs it in a simulator device, see
# slice.sh), e.g. by run-probe.sh. <wasm-threads-sdk dir> is swifthost/wasm-threads-sdk (test/p*.swift, tailcall/test/*.swift).
# Writes <work-dir>/logs/<name>.log and prints one table row per source.
set -uo pipefail
PROBE=$1 SDK=$2 WORK=$(mkdir -p "$3" && cd "$3" && pwd) TREE=$4
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/slice.sh"
LOGS=$WORK/logs BROKEN=$WORK/broken
mkdir -p "$LOGS" "$BROKEN" "$WORK/mc"
T=$TREE/test TC=$TREE/tailcall/test

check() { # <log> <step>: pass|fail|-
  local l; l=$(grep "^step=$2#1 " "$1" | head -1)
  case "$l" in *"check=pass"*) echo pass;; *"check=fail"*) echo FAIL;; *) echo -;; esac
}
rows=0 bad=0
row() { # <name> <diag-expect or ""> <primary> <extra files...> -- <probe options...>
  local name=$1 dexp=$2 primary=$3; shift 3
  local files=("$primary"); while [ "$1" != -- ]; do files+=("$1"); shift; done; shift
  local args=(--yaml-dir "$HERE" --sdk "$SDK" --mc "$WORK/mc" --extra-arg -parse-as-library --primary "$primary")
  for f in "${files[@]}"; do args+=(--file "$f"); done
  [ -n "$dexp" ] && args+=(--diag-expect "$dexp")
  slice_run "$PROBE" "${args[@]}" --steps init,open,diag,cursor,complete,close,shutdown "$@" > "$LOGS/$name.log" 2>&1
  local d c u; d=$(check "$LOGS/$name.log" diag); c=$(check "$LOGS/$name.log" complete); u=$(check "$LOGS/$name.log" cursor)
  printf '%-28s diag=%-5s complete=%-5s cursor=%-5s\n' "$name" "$d" "$c" "$u"
  rows=$((rows+1)); case "$d$c$u" in *FAIL*|*-*) bad=$((bad+1));; esac
}

row p1_taskgroup "" "$T/p1_taskgroup.swift" -- --complete-after "group." --complete-expect addTask --cursor-at withTaskGroup --cursor-expect withTaskGroup
row p2_atomic "" "$T/p2_atomic.swift" -- --complete-after "counter." --complete-expect add --cursor-at "counter.add" --cursor-expect counter
row p3_mutex "" "$T/p3_mutex.swift" -- --complete-after "shared." --complete-expect withLock --cursor-at "shared.withLock" --cursor-expect shared
row p4_foundation "" "$T/p4_foundation.swift" -- --complete-after "enc." --complete-expect encode --cursor-at "Item:" --cursor-expect Item
row pa_await_loop "" "$TC/pa_await_loop.swift" "$TC/common.swift" -- --complete-at "trivial(s)" --complete-expect trivial --cursor-at "trivial(s)" --cursor-expect trivial
row pb_actor_hops "" "$TC/pb_actor_hops.swift" "$TC/common.swift" -- --complete-at "c.bump" --complete-delta 2 --complete-expect bump --cursor-at "c.bump" --cursor-delta 2 --cursor-expect bump
row pe_patterns "" "$TC/pe_patterns.swift" "$TC/common.swift" -- --complete-at "t.step" --complete-delta 2 --complete-expect step --cursor-at "protocol Stepper" --cursor-delta 9 --cursor-expect Stepper
row pf_coop_timer "" "$TC/pf_coop_timer.swift" -- --complete-after "clock." --complete-expect now --cursor-at "clock = " --cursor-expect clock

# Broken copies: same layout, one defect each. The completion/cursor columns still apply.
sed 's/let sum = await/let sum: String = await/' "$T/p1_taskgroup.swift" > "$BROKEN/p1_taskgroup.swift"
sed 's/shared.withLock { \$0 += 1 }/sharedMissing.withLock { $0 += 1 }/' "$T/p3_mutex.swift" > "$BROKEN/p3_mutex.swift"
sed 's/^import Foundation//' "$T/p4_foundation.swift" > "$BROKEN/p4_foundation.swift"
sed 's/s = await trivial(s)/s = await trivial(s, 1)/' "$TC/pa_await_loop.swift" > "$BROKEN/pa_await_loop.swift"
row broken_p1_type "cannot convert value of type 'Int' to closure result type 'String'" "$BROKEN/p1_taskgroup.swift" -- --complete-after "group." --complete-expect addTask --cursor-at withTaskGroup --cursor-expect withTaskGroup
row broken_p3_undefined "cannot find 'sharedMissing' in scope" "$BROKEN/p3_mutex.swift" -- --complete-after "shared." --complete-expect withLock --cursor-at "Mutex<Int>" --cursor-expect Mutex
row broken_p4_noimport "cannot find 'JSONEncoder' in scope" "$BROKEN/p4_foundation.swift" -- --complete-at "Item(name" --complete-expect Item --cursor-at "Item:" --cursor-expect Item
row broken_pa_arity "extra argument" "$BROKEN/pa_await_loop.swift" "$TC/common.swift" -- --complete-at "trivial(s" --complete-expect trivial --cursor-at "trivial(s" --cursor-expect trivial
echo "rows=$rows rows_with_fail_or_missing=$bad"
[ "$bad" = 0 ]
