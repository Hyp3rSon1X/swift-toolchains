# Sourced by the test scripts (test/run-smoke.sh, test/run-cancel-stages.sh and, through sourcekitd/slice.sh, the
# sourcekitd tests): runs their test processes so that stopping a script stops them, and gives the simulator runs
# their device.
#
#   proc_run CMD...             runs CMD (a command or a shell function) in a process group of its own and waits
#                               for it; returns CMD's exit status.
#   sim_device_create <prefix>  creates and boots an iPhone device named <prefix>-<pid of the script> and sets
#                               SIM_UDID; the device is shut down and deleted when the script exits. With
#                               SWIFTHOST_SIM_UDID set, uses that booted device instead and leaves it alone.
#   sim_device_share <prefix>   sim_device_create, and exports SWIFTHOST_SIM_UDID: the test scripts this script runs
#                               use the same device, one after the other, and this script deletes it at the end.
#
# On HUP, INT and TERM a script that sourced this file sends the signal to the process group proc_run is waiting
# for (a test script in it does the same with its own), deletes its device and exits, so that a stopped run leaves
# no process and no booted device behind. A run that is killed outright cannot do this, so sim_device_create first
# deletes every device named <prefix>-<pid> whose process is gone.
SIM_UDID= SIM_OWNED= PROC_PGID=

proc_run() {
  local rc=0
  # Monitor mode puts the background job in a process group of its own, which proc_stop can signal as a whole.
  set -m
  "$@" < /dev/null &
  PROC_PGID=$!
  set +m
  wait "$PROC_PGID" || rc=$?
  PROC_PGID=
  return $rc
}

proc_stop() { # <signal>
  [ -z "$PROC_PGID" ] || kill -"$1" -- "-$PROC_PGID" 2>/dev/null || true
}

sim_device_cleanup() {
  [ -n "$SIM_UDID" ] || return 0
  local udid=$SIM_UDID owned=$SIM_OWNED
  SIM_UDID= SIM_OWNED=
  [ -n "$owned" ] || return 0
  xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
  xcrun simctl delete "$udid" >/dev/null 2>&1 || true
}

sim_on_signal() { # <signal> <exit status>
  trap '' HUP INT TERM
  proc_stop "$1"
  sim_device_cleanup
  exit "$2"
}

trap sim_device_cleanup EXIT
trap 'sim_on_signal HUP 129' HUP
trap 'sim_on_signal INT 130' INT
trap 'sim_on_signal TERM 143' TERM

sim_device_reap() { # <prefix>
  local udid name pid
  while read -r udid name; do
    pid=${name##*-}
    ps -p "$pid" >/dev/null 2>&1 && continue
    echo "deleting simulator device $name ($udid) left by a run that is gone" >&2
    xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
    xcrun simctl delete "$udid" >/dev/null 2>&1 || true
  done < <(xcrun simctl list devices -j | python3 -I -c '
import json, re, sys
pattern = re.escape(sys.argv[1]) + r"-[0-9]+"
for devices in json.load(sys.stdin)["devices"].values():
    for d in devices:
        if re.fullmatch(pattern, d["name"]):
            print(d["udid"], d["name"])' "$1")
}

sim_device_create() { # <prefix>
  local runtime devtype
  if [ -n "${SWIFTHOST_SIM_UDID:-}" ]; then
    SIM_UDID=$SWIFTHOST_SIM_UDID
    echo "simulator device: the shared one, $SIM_UDID" >&2
    return 0
  fi
  sim_device_reap "$1"
  # Among the available iOS runtimes that support more than one iPhone (some support a single device, and simctl
  # create rejects others), the newest version, then the one that supports the most device types.
  read -r runtime devtype < <(xcrun simctl list runtimes -j | python3 -I -c '
import json, sys
iphones = lambda r: [d["identifier"] for d in r.get("supportedDeviceTypes", []) if d["name"].startswith("iPhone")]
rs = [r for r in json.load(sys.stdin)["runtimes"] if r["platform"] == "iOS" and r["isAvailable"] and len(iphones(r)) > 1]
if not rs:
    sys.exit("no available iOS simulator runtime that supports iPhones")
r = max(rs, key=lambda r: ([int(x) for x in r["version"].split(".")], len(r["supportedDeviceTypes"])))
print(r["identifier"], iphones(r)[0])') || true
  if [ -z "${runtime:-}" ] || [ -z "${devtype:-}" ]; then
    echo "no usable iOS simulator runtime" >&2
    return 2
  fi
  SIM_UDID=$(xcrun simctl create "$1-$$" "$devtype" "$runtime") || return 2
  SIM_OWNED=1
  xcrun simctl boot "$SIM_UDID" && xcrun simctl bootstatus "$SIM_UDID" >/dev/null || return 2
  echo "simulator device: $1-$$ $runtime $devtype" >&2
}

sim_device_share() { # <prefix>
  sim_device_create "$1" || return
  export SWIFTHOST_SIM_UDID=$SIM_UDID
}
