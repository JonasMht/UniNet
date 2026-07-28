#!/usr/bin/env bash
# UniNet demo: three devices finding each other, with nothing configured.
#
#     ./scripts/demo.sh [seconds]
#
# Starts three peers in one realm, lets them discover each other and exchange
# messages, then shuts them down. Nobody types an address anywhere.
#
# The same three commands work on three separate machines on the same network -
# that is the point. Running them here just makes the demo self-contained.
set -euo pipefail

DURATION="${1:-12}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Find the built demo, wherever the user configured their build directory.
DEMO=""
# "$HERE"/build*/ used to be in this list, which picked up whatever old binary
# happened to be in a build2/ or build-android/ tree next door - an odd failure
# to debug, because the demo that ran was not the code you just changed.
for candidate in "$HERE/build/uninet-demo" "$HERE/build/uninet_demo" \
                 "$HERE/build/Release/uninet-demo.exe" "$HERE/build-native/uninet-demo"; do
    [ -x "$candidate" ] && { DEMO="$candidate"; break; }
done
if [ -z "$DEMO" ]; then
    echo "Could not find uninet-demo. Build it first:" >&2
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
    exit 1
fi

# A realm unique to this run, so the demo cannot join, or disturb, a real
# session that happens to be on the same network.
REALM="uninet-demo-$$"
LOGDIR="$(mktemp -d)"
PIDS=()

# Stopping the devices and throwing away their logs are two different things.
# They used to be one function, which the main flow called before printing the
# logs, so the demo deleted its own output and then printed nothing but "cat:
# No such file or directory" three times.
stop_devices() {
    for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
cleanup() {
    stop_devices
    rm -rf "$LOGDIR"
}
trap cleanup EXIT INT TERM

echo "UniNet demo: three devices, zero configuration"
echo "realm: $REALM   duration: ${DURATION}s"
echo

start() {  # start <name> <role>
    "$DEMO" "$1" --role "$2" --realm "$REALM" --quiet > "$LOGDIR/$2.log" 2>&1 &
    PIDS+=($!)
}

start "Recorder" server
sleep 1
start "Headset"        headset
sleep 1
start "Laptop"   viewer

sleep "$DURATION"
stop_devices
sleep 0.5

for role in server headset viewer; do
    echo "──────────── $role ────────────"
    cat "$LOGDIR/$role.log"
    echo
done

rm -rf "$LOGDIR"
echo "Each device found the others without being told any address."
