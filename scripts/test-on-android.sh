#!/usr/bin/env bash
# Run UniNet's test suite on a real Android device over USB.
#
#     ./scripts/build-for-android.sh      # first: cross-compile for arm64-v8a
#     ./scripts/test-on-android.sh        # then: push and run on the device
#
# Two things are tested, and they are different questions:
#
#   1. **On the device.** The codec, the C ABI and discovery between two nodes
#      inside the phone. This is UniNet running on ARM64 Android for real, not a
#      cross-compile that merely linked.
#
#   2. **Across the USB cable.** A node on the device and a node on this machine
#      finding each other with no Wi-Fi involved, using the rendezvous endpoint
#      the README describes for links without multicast. adb's port forwarding is
#      what carries it, which is exactly how a tethered headset works.
#
# The device needs USB debugging on (Settings > Developer options) and this
# machine authorised on it. Nothing is installed: the binaries run from
# /data/local/tmp and are deleted afterwards. No root needed.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE=/data/local/tmp/uninet-test
# BUILD is chosen once the device's ABI is known, below.

ADB="${ADB:-}"
if [ -z "$ADB" ]; then
    ADB="$(command -v adb 2>/dev/null)"
fi
if [ -z "$ADB" ]; then
    ADB="$(ls -d "$HOME"/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/SDK/platform-tools/adb 2>/dev/null | head -1)"
fi
[ -x "$ADB" ] || { echo "adb not found. Install android-sdk-platform-tools, or set ADB=." >&2; exit 2; }

STATE="$("$ADB" get-state 2>&1)"
if [ "$STATE" != "device" ]; then
    "$ADB" devices -l
    # An emulator has no USB debugging to enable, so do not send the reader off
    # to look for a setting that does not exist there.
    if [ -n "${ANDROID_SERIAL:-}" ] && case "$ANDROID_SERIAL" in emulator-*) true;; *) false;; esac; then
        echo "" >&2
        echo "$ANDROID_SERIAL is not ready (state: $STATE)." >&2
        echo "If it is still booting, wait; scripts/test-on-emulator.sh does that for you." >&2
        exit 2
    fi
    cat >&2 <<'EOF'

No authorised device. On the phone or headset:
  1. Settings > About > Software information: tap "Build number" seven times.
  2. Settings > Developer options: turn on "USB debugging".
  3. Accept the "Allow USB debugging?" prompt that appears when you plug in.
     Tick "Always allow from this computer".

"unauthorized" in the list above means step 3 is still pending.
EOF
    exit 2
fi

MODEL="$("$ADB" shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
REL="$("$ADB" shell getprop ro.build.version.release 2>/dev/null | tr -d '\r')"
ABI="$("$ADB" shell getprop ro.product.cpu.abi 2>/dev/null | tr -d '\r')"
echo "device : $MODEL, Android $REL, $ABI"

# Match the build to the device rather than assuming a phone. An x86_64
# emulator will happily RUN arm64 binaries through its translation layer, which
# makes this look like it works, but a dynamically linked one cannot load its
# arm64 .so and fails with "library libuninet_c.so not found" - a message that
# suggests a missing file rather than the wrong architecture.
BUILD="$HERE/build-android"
[ "$ABI" != "arm64-v8a" ] && BUILD="$HERE/build-android-$ABI"
if [ ! -f "$BUILD/test_network" ]; then
    echo "No build for $ABI at $BUILD." >&2
    echo "Build one:  ./scripts/build-for-android.sh \"\" $ABI" >&2
    exit 2
fi
echo "build  : $BUILD"
echo

FAILED=0
note() { echo; echo "──── $* ────"; }

cleanup() {
    "$ADB" shell "rm -rf $REMOTE" >/dev/null 2>&1
    "$ADB" reverse --remove-all >/dev/null 2>&1
    "$ADB" forward --remove-all >/dev/null 2>&1
}
trap cleanup EXIT

"$ADB" shell "rm -rf $REMOTE && mkdir -p $REMOTE" >/dev/null
for f in test_roundtrip test_network test_cabi uninet-demo libuninet_c.so; do
    [ -f "$BUILD/$f" ] && "$ADB" push "$BUILD/$f" "$REMOTE/" >/dev/null 2>&1
done
"$ADB" shell "chmod 755 $REMOTE/*" >/dev/null

run_remote() {   # run_remote <label> <command...>
    local label="$1"; shift
    note "$label"
    # The exit status has to come back over `adb shell`, which does not forward
    # it on older devices: echoing a marker is the portable way to read it.
    local out
    out="$("$ADB" shell "cd $REMOTE && LD_LIBRARY_PATH=$REMOTE $* ; echo RC=\$?" 2>&1 | tr -d '\r')"
    echo "$out" | grep -v '^RC='
    local rc="${out##*RC=}"
    if [ "$rc" != "0" ]; then
        echo "  -> FAILED (exit $rc)"
        FAILED=1
    else
        echo "  -> PASS"
    fi
}

# ── 1. on the device ─────────────────────────────────────────────────────────
run_remote "codec, compression, framing, hostile input" ./test_roundtrip
run_remote "C ABI, compiled as C" ./test_cabi
run_remote "discovery and messaging between two nodes on the device" ./test_network

# ── 2. across the USB cable ──────────────────────────────────────────────────
# Wi-Fi is not involved. Zyre's UDP beacon cannot cross USB, so both sides use
# the rendezvous endpoint instead, and adb carries the three TCP connections:
#
#   reverse 31337  device -> host   the rendezvous itself
#   reverse 31339  device -> host   the host node's data endpoint
#   forward 31338  host   -> device the device node's data endpoint
#
# Each side therefore advertises a 127.0.0.1 address, which resolves correctly
# on the other side because adb is forwarding that exact port.
note "discovery across the USB cable, with no network"
if [ ! -f "$BUILD/uninet-demo" ] || [ ! -x "$HERE/build/uninet-demo" ]; then
    # Counted as a failure, not a quiet skip. This is the only cross-device
    # check in the suite, and "PASS" printed while it never ran is the kind of
    # result that gets trusted.
    echo "  FAILED: uninet-demo is missing for the device ($BUILD) or this"
    echo "  machine ($HERE/build). Build both:"
    echo "      ./scripts/build-for-android.sh"
    echo "      cmake --build build -j --target uninet_demo"
    FAILED=1
else
    "$ADB" reverse tcp:31337 tcp:31337 >/dev/null
    "$ADB" reverse tcp:31339 tcp:31339 >/dev/null
    "$ADB" forward tcp:31338 tcp:31338 >/dev/null

    REALM="usb-$$"
    HOSTLOG="$(mktemp)"
    "$HERE/build/uninet-demo" "Workstation" --role server --realm "$REALM" \
        --gossip-bind 'tcp://127.0.0.1:31337' \
        --endpoint 'tcp://127.0.0.1:31339' >"$HOSTLOG" 2>&1 &
    HOSTPID=$!
    sleep 2

    # Bound to 127.0.0.1, not 0.0.0.0. A node advertises the endpoint it bound,
    # so binding the wildcard makes it tell peers "reach me at 0.0.0.0", which
    # is not an address anything can dial. Binding the loopback address makes
    # the advertisement literally true on the other side of the cable, because
    # that is the port adb is forwarding. This is also why no --advertise is
    # needed: overriding the advertised address requires a draft-enabled Zyre,
    # and this way needs nothing.
    DEVLOG="$("$ADB" shell "cd $REMOTE && LD_LIBRARY_PATH=$REMOTE timeout 12 ./uninet-demo 'Phone' \
        --role headset --realm '$REALM' \
        --gossip-connect 'tcp://127.0.0.1:31337' \
        --endpoint 'tcp://127.0.0.1:31338'" 2>&1 | tr -d '\r')"

    sleep 1
    kill "$HOSTPID" 2>/dev/null; wait "$HOSTPID" 2>/dev/null

    echo "--- on the device ---"; echo "$DEVLOG" | head -12
    echo "--- on this machine ---"; head -12 "$HOSTLOG"

    # Presence is not enough. Discovery is bidirectional even when the return
    # path is broken, so an earlier version of this check passed while every
    # message from this machine to the device was being sent to 0.0.0.0 and
    # silently dropped. Both directions have to carry an actual message.
    ok=1
    grep -q "JOINED  Workstation" <<<"$DEVLOG" || { echo "  the device never saw the workstation"; ok=0; }
    grep -q "JOINED  Phone" "$HOSTLOG"         || { echo "  the workstation never saw the device"; ok=0; }
    grep -q "MESSAGE on demo.hello" <<<"$DEVLOG" || { echo "  no message reached the device"; ok=0; }
    grep -q "MESSAGE on demo.hello" "$HOSTLOG"   || { echo "  no message reached this machine"; ok=0; }
    if [ "$ok" -eq 1 ]; then
        echo "  -> PASS: both found each other AND messages crossed both ways over USB"
    else
        echo "  -> FAILED"
        FAILED=1
    fi
    rm -f "$HOSTLOG"
fi

echo
if [ "$FAILED" -ne 0 ]; then
    echo "=== RESULT: FAIL ==="
    exit 1
fi
echo "=== RESULT: PASS on $MODEL (Android $REL, $ABI) ==="
