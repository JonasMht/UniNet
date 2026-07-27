#!/usr/bin/env bash
# Run the Android suite on an emulator, with no hardware attached.
#
#     ./scripts/test-on-emulator.sh              boot, test, shut down
#     ./scripts/test-on-emulator.sh --keep       leave it running afterwards
#
# scripts/test-on-android.sh needs a device plugged in. This provides one. It is
# the same suite - the codec, the C ABI, discovery between two nodes on the
# device, and two nodes finding each other across the adb link - run against a
# Google system image instead of a phone.
#
# WHAT IT DOES AND DOES NOT PROVE. It is a real Android userspace: the same
# bionic libc, the same networking stack, the same restrictions on
# /data/local/tmp. It is NOT a Quest, it is x86_64 rather than ARM, and its
# Wi-Fi is emulated, so it cannot tell you anything about the multicast
# filtering that makes a MulticastLock necessary on real hardware. Treat it as
# the check that runs on every machine, and the physical device as the one that
# settles Wi-Fi questions.
#
# Everything it installs goes under a scratch SDK root, so an existing Android
# or Unity installation is left alone.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEEP=0
[ "${1:-}" = "--keep" ] && KEEP=1

: "${UNINET_EMU_HOME:=${TMPDIR:-/tmp}/uninet-emulator}"
SDK="$UNINET_EMU_HOME/sdk"
AVD_HOME="$UNINET_EMU_HOME/avd"
API=34
IMAGE="system-images;android-$API;google_apis;x86_64"
ABI=x86_64
PORT=5560
SERIAL="emulator-$PORT"

# A JDK is needed only by sdkmanager. Unity bundles one; so does any JDK 11+.
JAVA_HOME="${JAVA_HOME:-$(ls -d "$HOME"/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/OpenJDK 2>/dev/null | head -1)}"
export JAVA_HOME
SDKMANAGER="${UNINET_SDKMANAGER:-$(ls "$HOME"/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/SDK/cmdline-tools/*/bin/sdkmanager 2>/dev/null | head -1)}"

need_download=0
[ -x "$SDK/emulator/emulator" ] || need_download=1
[ -d "$SDK/system-images/android-$API/google_apis/$ABI" ] || need_download=1

if [ "$need_download" = "1" ]; then
    if [ -z "$SDKMANAGER" ] || [ ! -x "$SDKMANAGER" ]; then
        cat >&2 <<EOF
The emulator is not installed and sdkmanager was not found.

Install Android Build Support in Unity Hub (which bundles both), or point
UNINET_SDKMANAGER at an sdkmanager and re-run. Roughly 6 GB is downloaded once
into $SDK.
EOF
        exit 2
    fi
    echo "downloading the emulator and an Android $API image (about 6 GB, once)..."
    mkdir -p "$SDK"
    yes | "$SDKMANAGER" --sdk_root="$SDK" emulator platform-tools "platforms;android-$API" "$IMAGE" \
        > "$UNINET_EMU_HOME/sdk-install.log" 2>&1 \
        || { echo "download failed, see $UNINET_EMU_HOME/sdk-install.log" >&2; exit 1; }
fi

ADB="${ADB:-$SDK/platform-tools/adb}"
[ -x "$ADB" ] || ADB="$(command -v adb 2>/dev/null)"
[ -x "$ADB" ] || { echo "adb not found" >&2; exit 2; }

# ── the AVD ──────────────────────────────────────────────────────────────────
# Written directly rather than through avdmanager, which needs a JDK 17 that a
# machine set up for Unity (JDK 11) does not have. An AVD is two ini files.
mkdir -p "$AVD_HOME/uninet-test.avd"
cat > "$AVD_HOME/uninet-test.ini" <<EOF
avd.ini.encoding=UTF-8
path=$AVD_HOME/uninet-test.avd
path.rel=uninet-test.avd
target=android-$API
EOF
cat > "$AVD_HOME/uninet-test.avd/config.ini" <<EOF
AvdId=uninet-test
avd.ini.encoding=UTF-8
avd.ini.displayname=uninet-test
abi.type=$ABI
tag.id=google_apis
tag.display=Google APIs
image.sysdir.1=system-images/android-$API/google_apis/$ABI/
hw.cpu.arch=$ABI
hw.ramSize=3072
hw.lcd.density=420
hw.lcd.width=1080
hw.lcd.height=1920
hw.gpu.enabled=yes
hw.gpu.mode=swiftshader_indirect
hw.keyboard=yes
disk.dataPartition.size=4G
image.androidVersion.api=$API
EOF

# ── the native build for the emulator's architecture ─────────────────────────
if [ ! -f "$HERE/build-android-$ABI/test_network" ]; then
    echo "building UniNet for Android $ABI..."
    "$HERE/scripts/build-for-android.sh" "" "$ABI" > "$UNINET_EMU_HOME/build.log" 2>&1 \
        || { echo "build failed, see $UNINET_EMU_HOME/build.log" >&2; exit 1; }
fi

# ── boot ─────────────────────────────────────────────────────────────────────
export ANDROID_SDK_ROOT="$SDK"
export ANDROID_AVD_HOME="$AVD_HOME"
export ANDROID_EMULATOR_HOME="$UNINET_EMU_HOME/home"
mkdir -p "$ANDROID_EMULATOR_HOME"

EMU_PID=""
shutdown_emulator() {
    [ "$KEEP" = "1" ] && { echo "emulator left running as $SERIAL"; return; }
    "$ADB" -s "$SERIAL" emu kill >/dev/null 2>&1
    [ -n "$EMU_PID" ] && kill "$EMU_PID" 2>/dev/null
    wait "$EMU_PID" 2>/dev/null
}
trap shutdown_emulator EXIT

# "Is one already running" has to mean "is one already USABLE". A previous
# emulator that is still shutting down is listed by `adb devices` for several
# seconds, and treating it as running skipped the boot and ran the whole suite
# against a device that was disappearing: two runs in three failed that way,
# reporting "No authorised device" as though the emulator had never existed.
reusable=0
if [ "$("$ADB" devices | grep -c "^$SERIAL")" -gt 0 ]; then
    if [ "$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; then
        reusable=1
        echo "reusing the emulator already running as $SERIAL"
    else
        echo "an emulator on $PORT is not usable; replacing it"
        "$ADB" -s "$SERIAL" emu kill >/dev/null 2>&1
        for _ in $(seq 1 30); do
            [ "$("$ADB" devices | grep -c "^$SERIAL")" -eq 0 ] && break
            sleep 1
        done
    fi
fi

if [ "$reusable" -eq 0 ]; then
    echo "booting the emulator..."
    # -no-window because this has to work over ssh and in CI. KVM makes it fast;
    # without /dev/kvm it still boots, just slowly.
    "$SDK/emulator/emulator" -avd uninet-test -no-window -no-audio -no-boot-anim \
        -gpu swiftshader_indirect -no-snapshot -wipe-data -port "$PORT" \
        > "$UNINET_EMU_HOME/emulator.log" 2>&1 &
    EMU_PID=$!

    echo -n "waiting for boot"
    for _ in $(seq 1 120); do
        state="$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
        [ "$state" = "1" ] && break
        echo -n "."
        sleep 2
    done
    echo
    if [ "$("$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" != "1" ]; then
        echo "the emulator did not finish booting; see $UNINET_EMU_HOME/emulator.log" >&2
        exit 1
    fi

    # boot_completed is not the same as ready. On a freshly wiped image it goes
    # to 1 while the network stack is still coming up, and a discovery test
    # started in that window fails for no reason of its own: seen once, and once
    # is enough to make a suite untrustworthy. Wait for an actual routable
    # address, then let the animation finish.
    echo -n "waiting for the network"
    for _ in $(seq 1 60); do
        if "$ADB" -s "$SERIAL" shell ip -4 addr show 2>/dev/null \
             | grep -qE "inet (10|192|172)\."; then
            break
        fi
        echo -n "."
        sleep 1
    done
    echo
    for _ in $(seq 1 30); do
        [ "$("$ADB" -s "$SERIAL" shell getprop init.svc.bootanim 2>/dev/null | tr -d '\r')" = "stopped" ] && break
        sleep 1
    done
fi

# ── the suite ────────────────────────────────────────────────────────────────
echo
ADB="$ADB" ANDROID_SERIAL="$SERIAL" "$HERE/scripts/test-on-android.sh"
RC=$?
exit $RC
