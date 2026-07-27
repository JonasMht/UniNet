#!/usr/bin/env bash
# Run the workstation half of the UniNet Android demo, over the USB cable.
#
#     ./examples/android/usb-peer.sh          run a peer and wait for the tablet
#     ./examples/android/usb-peer.sh --test    launch the app too, then check it
#
# WHY ANY OF THIS IS NEEDED. UniNet normally discovers peers with a UDP beacon
# on the local link. A USB cable is not a link that carries multicast, so there
# is nothing to beacon over. Discovery therefore falls back to a rendezvous
# endpoint: one side binds it, the other dials it, and after that they exchange
# endpoints and talk directly.
#
# adb is what carries the three TCP connections across the cable:
#
#   reverse 31337   device -> host   the rendezvous
#   reverse 31339   device -> host   this machine's mailbox
#   forward 31338   host   -> device the tablet's mailbox
#
# Each side binds its mailbox on 127.0.0.1 and therefore advertises a 127.0.0.1
# address, which is correct on the other side precisely because adb is
# forwarding that port. Nothing needs to know an IP address, and no Wi-Fi is
# involved: this works with the tablet in flight mode.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
TEST=0
[ "${1:-}" = "--test" ] && TEST=1

ADB="${ADB:-$(command -v adb 2>/dev/null || true)}"
if [ -z "$ADB" ]; then
    ADB="$(ls -d "$HOME"/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/SDK/platform-tools/adb 2>/dev/null | head -1)"
fi
[ -x "$ADB" ] || { echo "adb not found. Install android-sdk-platform-tools, or set ADB=." >&2; exit 2; }

if [ "$("$ADB" get-state 2>&1)" != "device" ]; then
    "$ADB" devices -l
    cat >&2 <<'EOF'

No authorised device. On the tablet:
  Settings > About > Software information: tap "Build number" seven times,
  then Settings > Developer options > USB debugging, and accept the prompt.
EOF
    exit 2
fi

cleanup() {
    "$ADB" reverse --remove-all >/dev/null 2>&1
    "$ADB" forward --remove-all >/dev/null 2>&1
    [ -n "${PEER_PID:-}" ] && kill "$PEER_PID" 2>/dev/null
}
trap cleanup EXIT

echo "device : $("$ADB" shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
"$ADB" reverse tcp:31337 tcp:31337 >/dev/null
"$ADB" reverse tcp:31339 tcp:31339 >/dev/null
"$ADB" forward tcp:31338 tcp:31338 >/dev/null
echo "cable  : rendezvous 31337, mailboxes 31338 (device) / 31339 (here)"
echo

# The in-tree build first, so this never silently tests an old `pip install`.
export PYTHONPATH="$ROOT/python:${PYTHONPATH:-}"
if ! python3 -c "import uninet" 2>/dev/null; then
    echo "the Python binding is not importable." >&2
    echo "Build it:  cmake -S . -B build && cmake --build build -j" >&2
    exit 2
fi

if [ "$TEST" = "0" ]; then
    echo "Now open \"UniNet Demo\" on the tablet and tap \"Connect over USB\"."
    echo "Ctrl+C to stop."
    echo
    exec python3 "$HERE/usb_peer.py"
fi

# ── automated ────────────────────────────────────────────────────────────────
# The app is normally driven by a finger; the intent extra stands in for one.
echo "starting the workstation peer..."
python3 "$HERE/usb_peer.py" --seconds 18 --expect-peer > /tmp/usb-peer.log 2>&1 &
PEER_PID=$!
sleep 2

echo "launching the app on the tablet..."
"$ADB" logcat -c >/dev/null 2>&1
"$ADB" shell am start -n org.uninet.demo/.MainActivity --es mode usb >/dev/null 2>&1
sleep 16

DEVICE_LOG="$("$ADB" logcat -d -s UniNetDemo 2>/dev/null | tr -d '\r')"
wait "$PEER_PID"; PEER_RC=$?
PEER_PID=""

echo
echo "──── the workstation saw ────"
grep -E "^  [+<>-]|^OK|^FAIL" /tmp/usb-peer.log | head -14
echo
echo "──── the tablet saw ────"
grep -oE "(joined over USB.*|[+<>-] .*)" <<<"$DEVICE_LOG" | head -14

FAILED=0
[ "$PEER_RC" -eq 0 ] || FAILED=1
# The device half has to be checked separately: the workstation receiving
# messages says nothing about whether its own reached the tablet, and that is
# the direction that has been silently broken before.
grep -q "joined over USB" <<<"$DEVICE_LOG" || { echo "the app never joined"; FAILED=1; }
grep -q "< chat.room" <<<"$DEVICE_LOG"    || { echo "no message reached the tablet"; FAILED=1; }

echo
if [ "$FAILED" -ne 0 ]; then
    echo "=== USB LINK TEST: FAIL ==="
    exit 1
fi
echo "=== USB LINK TEST: PASS (messages crossed both ways over the cable) ==="
