# Sourced by the test scripts that run in a temporary iOS simulator device (test/run-smoke.sh and, through
# sourcekitd/slice.sh, the sourcekitd tests).
#
#   sim_device_create <prefix>  creates and boots an iPhone device named <prefix>-<pid of the script> and sets
#                               SIM_UDID. The device is shut down and deleted when the script exits, also on
#                               HUP, INT and TERM.
#
# A run that is killed outright cannot delete its device, and a booted device keeps its processes running. So
# sim_device_create first deletes every device named <prefix>-<pid> whose process is gone.
SIM_UDID=

sim_device_cleanup() {
  [ -n "$SIM_UDID" ] || return 0
  local udid=$SIM_UDID
  SIM_UDID=
  xcrun simctl shutdown "$udid" >/dev/null 2>&1 || true
  xcrun simctl delete "$udid" >/dev/null 2>&1 || true
}

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
  trap sim_device_cleanup EXIT
  trap 'sim_device_cleanup; exit 129' HUP
  trap 'sim_device_cleanup; exit 130' INT
  trap 'sim_device_cleanup; exit 143' TERM
  SIM_UDID=$(xcrun simctl create "$1-$$" "$devtype" "$runtime") || return 2
  xcrun simctl boot "$SIM_UDID" && xcrun simctl bootstatus "$SIM_UDID" >/dev/null || return 2
  echo "simulator device: $1-$$ $runtime $devtype" >&2
}
