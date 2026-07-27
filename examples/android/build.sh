#!/usr/bin/env bash
# Build (and optionally install) the UniNet Android demo.
#
#     ./examples/android/build.sh            build the APK
#     ./examples/android/build.sh --install  build it and push it to the device
#
# No Gradle and no Android Studio: this drives the SDK build tools directly,
# which keeps the whole app to two Java files, one C++ file and a manifest. It
# also means no Unity licence is involved.
#
# It needs an Android SDK (build-tools + a platform android.jar) and a JDK. The
# ones bundled with Unity are used when nothing else is found, because a machine
# doing Quest work already has them.
#
# Prerequisite: the native library, from ../../scripts/build-for-android.sh.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/build"
INSTALL=0
[ "${1:-}" = "--install" ] && INSTALL=1

# ── toolchain ────────────────────────────────────────────────────────────────
UNITY_ANDROID="$(ls -d "$HOME"/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer 2>/dev/null | sort -V | tail -1)"
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$UNITY_ANDROID/SDK}}"
NDK="${ANDROID_NDK_ROOT:-$UNITY_ANDROID/NDK}"
JAVA_HOME="${JAVA_HOME:-$UNITY_ANDROID/OpenJDK}"

BT="$(ls -d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -1)"
PLATFORM="$(ls -d "$SDK"/platforms/android-* 2>/dev/null | sort -V | tail -1)"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"

for needed in "$BT/aapt2" "$BT/d8" "$BT/apksigner" "$BT/zipalign" \
              "$PLATFORM/android.jar" "$JAVA_HOME/bin/javac" \
              "$TOOLCHAIN/bin/clang++"; do
    [ -e "$needed" ] || { echo "missing: $needed" >&2
        echo "Set ANDROID_SDK_ROOT / ANDROID_NDK_ROOT / JAVA_HOME, or install" >&2
        echo "Unity's Android Build Support module." >&2; exit 2; }
done

ABI=arm64-v8a
API=24
CABI="$ROOT/build-android/libuninet_c.so"
if [ ! -f "$CABI" ]; then
    echo "missing $CABI" >&2
    echo "Build it first:  ./scripts/build-for-android.sh" >&2
    exit 2
fi

echo "SDK      : $SDK"
echo "build    : $(basename "$BT"), $(basename "$PLATFORM")"
rm -rf "$OUT"; mkdir -p "$OUT/lib/$ABI" "$OUT/classes"

# ── 1. the JNI shim ─────────────────────────────────────────────────────────
# Linked against the C ABI shared library, exactly like the C# binding: this
# demo exercises the same artifact that ships, not a private static build.
echo "compiling the JNI shim..."
# -static-libstdc++ matters. Without it the NDK links the C++ runtime
# dynamically, the library then needs libc++_shared.so, and that file is not in
# the APK unless it is copied in deliberately. The app installs fine and dies on
# launch with
#     UnsatisfiedLinkError: dlopen failed: library "libc++_shared.so" not found
# libuninet_c.so is already built with the static runtime, so this just matches
# it and keeps the APK to the two libraries it actually ships.
"$TOOLCHAIN/bin/clang++" \
    --target=aarch64-linux-android$API \
    --sysroot="$TOOLCHAIN/sysroot" \
    -std=c++17 -O2 -fPIC -shared -fvisibility=hidden \
    -static-libstdc++ \
    -I"$ROOT/include" \
    -o "$OUT/lib/$ABI/libuninet_jni.so" \
    "$HERE/jni/uninet_jni.cpp" \
    "$CABI" -llog
cp "$CABI" "$OUT/lib/$ABI/"

# Checked here rather than discovered on the device. Every NEEDED entry must be
# either an Android system library or something present in the APK; anything
# else is a crash at launch, which is a slow way to learn about a link flag.
READELF="$TOOLCHAIN/bin/llvm-readelf"
for lib in "$OUT/lib/$ABI"/*.so; do
    while read -r need; do
        case "$need" in
            libc.so|libm.so|libdl.so|liblog.so|libz.so|libandroid.so) ;;
            libuninet_c.so) ;;   # shipped alongside, in the same directory
            *)
                echo "FAILED: $(basename "$lib") needs $need, which the APK does not ship." >&2
                echo "It would install and then crash on launch." >&2
                exit 1 ;;
        esac
    done < <("$READELF" -d "$lib" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
done

# ── 2. Java -> dex ───────────────────────────────────────────────────────────
echo "compiling Java..."
# android.jar goes on the CLASSPATH, not the bootclasspath. As a bootclasspath
# it replaces the JDK's core classes, and android.jar has no
# java.lang.invoke.LambdaMetafactory, so every lambda fails to compile with
# "Unable to find method metafactory". Android does not need it at runtime
# either: d8 desugars lambdas in the next step.
#
# Not piped into grep, either. A pipeline reports the exit status of its LAST
# command, so `javac ... | grep -v warning || true` returned success after a
# compile error, d8 dexed an empty directory, and the script cheerfully
# produced a signed APK with no application code in it.
if ! "$JAVA_HOME/bin/javac" -nowarn -source 8 -target 8 \
        -classpath "$PLATFORM/android.jar" \
        -d "$OUT/classes" \
        "$HERE"/java/org/uninet/demo/*.java 2>"$OUT/javac.log"; then
    grep -v "bootstrap class path\|source value 8\|target value 8" "$OUT/javac.log" >&2 || true
    echo "Java compilation failed" >&2
    exit 1
fi

CLASSES="$(find "$OUT/classes" -name '*.class' | wc -l)"
[ "$CLASSES" -gt 0 ] || { echo "javac produced no classes" >&2; exit 1; }
echo "  $CLASSES classes"

echo "dexing..."
"$BT/d8" --min-api $API --lib "$PLATFORM/android.jar" \
    --output "$OUT" $(find "$OUT/classes" -name '*.class') >/dev/null

# ── 3. package ───────────────────────────────────────────────────────────────
# aapt2 builds the APK skeleton from the manifest; the dex and the native
# libraries are added afterwards, which is all `zip` is needed for.
echo "packaging..."
"$BT/aapt2" link -o "$OUT/base.apk" \
    --manifest "$HERE/AndroidManifest.xml" \
    -I "$PLATFORM/android.jar" \
    --min-sdk-version $API --target-sdk-version 33 >/dev/null

cp "$OUT/base.apk" "$OUT/unaligned.apk"
(cd "$OUT" && zip -q "unaligned.apk" classes.dex "lib/$ABI/libuninet_c.so" "lib/$ABI/libuninet_jni.so")

"$BT/zipalign" -f 4 "$OUT/unaligned.apk" "$OUT/uninet-demo.apk"

# ── 4. sign ──────────────────────────────────────────────────────────────────
# A throwaway debug key. Android refuses to install an unsigned APK, and this
# is a demo, not something to publish.
#
# Kept OUTSIDE the build directory, which this script wipes on every run. A key
# regenerated each build changes the signature each build, and Android then
# refuses the upgrade with INSTALL_FAILED_UPDATE_INCOMPATIBLE, which reads as a
# broken APK rather than a new key.
KS="$HERE/.debug.keystore"
if [ ! -f "$KS" ]; then
    "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$KS" -storepass android \
        -keypass android -alias demo -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=UniNet Demo" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --v1-signing-enabled true --v2-signing-enabled true "$OUT/uninet-demo.apk"

echo
echo "built: $OUT/uninet-demo.apk ($(du -h "$OUT/uninet-demo.apk" | cut -f1))"

# ── 5. install ───────────────────────────────────────────────────────────────
if [ "$INSTALL" = "1" ]; then
    ADB="${ADB:-$(command -v adb || true)}"
    [ -n "$ADB" ] || ADB="$SDK/platform-tools/adb"
    [ -x "$ADB" ] || { echo "adb not found; set ADB=" >&2; exit 2; }
    echo "installing..."
    if ! "$ADB" install -r "$OUT/uninet-demo.apk" 2>&1 | tee /tmp/uninet-install.log \
         | grep -q "^Success"; then
        # A copy signed with a different key cannot be upgraded in place. That
        # is the normal case after the key above was regenerated once, so
        # replace it rather than leaving the user to decode the error.
        if grep -q "INSTALL_FAILED_UPDATE_INCOMPATIBLE" /tmp/uninet-install.log; then
            echo "  an incompatibly-signed copy is installed; replacing it"
            "$ADB" uninstall org.uninet.demo >/dev/null 2>&1
            "$ADB" install "$OUT/uninet-demo.apk" || exit 1
        else
            tail -5 /tmp/uninet-install.log >&2
            exit 1
        fi
    fi
    echo
    echo "Installed as \"UniNet Demo\". Launch it from the app drawer, or:"
    echo "  $ADB shell am start -n org.uninet.demo/.MainActivity"
    echo
    echo "For the USB button to find anything, run this on the workstation first:"
    echo "  ./examples/android/usb-peer.sh"
fi
