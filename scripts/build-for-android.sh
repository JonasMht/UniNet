#!/usr/bin/env bash
# Build libuninet_c.so for Android (Meta Quest and any other ARM64 device).
#
#     ./scripts/build-for-android.sh [/path/to/ndk] [abi]
#
# With no arguments it uses the NDK bundled with a Unity install, which is what
# a Unity project already needs anyway. abi defaults to arm64-v8a (Quest 2/3,
# Quest Pro, and every current Android headset and phone).
#
# Drop the result into your Unity project at
#     Assets/Plugins/Android/libs/arm64-v8a/libuninet_c.so
#
# Three things about this build are not obvious, and cost real time to find:
#
#  1. **Host pkg-config leaks into the cross build.** czmq's CMake probes with
#     pkg-config, finds the HOST libzmq, and adds /usr/include to an ARM64
#     compile. It fails with "gnu/stubs-32.h: file not found", which points
#     nowhere near the cause. PKG_CONFIG_LIBDIR is pinned to the Android prefix.
#
#  2. **czmq needs liblog.** It logs through __android_log_print, so the link
#     fails with an undefined symbol naming czmq rather than the missing
#     library. UniNet's CMakeLists links it whenever ANDROID is set.
#
#  3. **LZ4 is built from source, and must be.** liblz4 is not part of the NDK.
#     Leaving it out looks harmless and is not: the compression tier is chosen
#     by the SENDER and travels on the wire, so a device without LZ4 silently
#     discards every message from a desktop peer that has it. Discovery still
#     works, presence still works, the device can still send, and only the
#     inbound payloads vanish, with no error at either end. UniNet's CMake
#     fetches liblz4 when the system has none, so nothing extra is needed here.
set -euo pipefail

ABI="${2:-arm64-v8a}"
API=24                      # Android 7.0; Quest is well above this

NDK="${1:-}"
if [ -z "$NDK" ]; then
    NDK="$(ls -d "$HOME"/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/NDK 2>/dev/null | head -1)"
fi
if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
    cat >&2 <<USAGE
usage: $0 [/path/to/android-ndk] [abi]

No NDK found. Either pass one, or install the Android Build Support module in
Unity Hub, which bundles it at
  ~/Unity/Hub/Editor/<version>/Editor/Data/PlaybackEngines/AndroidPlayer/NDK
USAGE
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$HERE/build-android/deps"
PREFIX="$WORK/prefix"
mkdir -p "$PREFIX/lib/pkgconfig" "$PREFIX/include"

TC="$NDK/build/cmake/android.toolchain.cmake"
COMMON=(-DCMAKE_TOOLCHAIN_FILE="$TC" -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API"
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX"
        -DCMAKE_FIND_ROOT_PATH="$PREFIX")

# See note 1: never let pkg-config see the host's libraries.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"

echo "NDK  : $NDK"
echo "ABI  : $ABI (API $API)"
echo

fetch() {   # fetch <name> <tag> <url>
    [ -d "$WORK/$1" ] || git clone --depth 1 -b "$2" "$3" "$WORK/$1" >/dev/null 2>&1
}

if [ ! -f "$PREFIX/lib/libz.a" ]; then
    echo "building zlib..."
    fetch zlib v1.3.1 https://github.com/madler/zlib.git
    cmake -S "$WORK/zlib" -B "$WORK/zlib/b" "${COMMON[@]}" >/dev/null
    cmake --build "$WORK/zlib/b" -j"$(nproc)" >/dev/null
    cmake --install "$WORK/zlib/b" >/dev/null
fi

if [ ! -f "$PREFIX/lib/libzmq.a" ]; then
    echo "building libzmq..."
    fetch libzmq v4.3.5 https://github.com/zeromq/libzmq.git
    cmake -S "$WORK/libzmq" -B "$WORK/libzmq/b" "${COMMON[@]}" \
          -DBUILD_TESTS=OFF -DBUILD_SHARED=OFF -DBUILD_STATIC=ON \
          -DWITH_PERF_TOOL=OFF -DWITH_DOC=OFF -DENABLE_CURVE=OFF -DWITH_LIBBSD=OFF >/dev/null
    cmake --build "$WORK/libzmq/b" -j"$(nproc)" >/dev/null
    cmake --install "$WORK/libzmq/b" >/dev/null
fi

if [ ! -f "$PREFIX/lib/libczmq.a" ]; then
    echo "building czmq..."
    fetch czmq v4.2.1 https://github.com/zeromq/czmq.git
    # Only the library target: czmq's tools do not cross-link and are unused.
    cmake -S "$WORK/czmq" -B "$WORK/czmq/b" "${COMMON[@]}" \
          -DCZMQ_BUILD_SHARED=OFF -DCZMQ_BUILD_STATIC=ON -DBUILD_TESTING=OFF \
          -DLIBZMQ_LIBRARIES="$PREFIX/lib/libzmq.a" \
          -DLIBZMQ_INCLUDE_DIRS="$PREFIX/include" >/dev/null
    cmake --build "$WORK/czmq/b" -j"$(nproc)" --target czmq-static >/dev/null
    cp "$WORK/czmq/b/libczmq.a" "$PREFIX/lib/"
    cp "$WORK/czmq/include/"*.h "$PREFIX/include/"
fi

if [ ! -f "$PREFIX/lib/libzyre.a" ]; then
    echo "building zyre..."
    fetch zyre v2.0.1 https://github.com/zeromq/zyre.git
    cmake -S "$WORK/zyre" -B "$WORK/zyre/b" "${COMMON[@]}" \
          -DZYRE_BUILD_SHARED=OFF -DZYRE_BUILD_STATIC=ON -DBUILD_TESTING=OFF \
          -DCZMQ_LIBRARIES="$PREFIX/lib/libczmq.a" -DCZMQ_INCLUDE_DIRS="$PREFIX/include" \
          -DLIBZMQ_LIBRARIES="$PREFIX/lib/libzmq.a" -DLIBZMQ_INCLUDE_DIRS="$PREFIX/include" >/dev/null
    cmake --build "$WORK/zyre/b" -j"$(nproc)" --target zyre-static >/dev/null
    cp "$WORK/zyre/b/libzyre.a" "$PREFIX/lib/"
    cp "$WORK/zyre/include/"*.h "$PREFIX/include/"
fi

# czmq and zyre only write their .pc during `install`, which is skipped above.
for spec in "libzmq:4.3.5:-lzmq" "libczmq:4.2.1:-lczmq -lzmq -lz" "libzyre:2.0.1:-lzyre -lczmq -lzmq -lz"; do
    name="${spec%%:*}"; rest="${spec#*:}"; ver="${rest%%:*}"; libs="${rest#*:}"
    printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: %s\nDescription: %s\nVersion: %s\nLibs: -L${libdir} %s\nCflags: -I${includedir}\n' \
        "$PREFIX" "$name" "$name" "$ver" "$libs" > "$PREFIX/lib/pkgconfig/$name.pc"
done

echo "building UniNet..."
cmake -S "$HERE" -B "$HERE/build-android" "${COMMON[@]}" \
      -DUNINET_BUILD_CABI=ON \
      -DZLIB_LIBRARY="$PREFIX/lib/libz.a" -DZLIB_INCLUDE_DIR="$PREFIX/include" >/dev/null
# The test binaries too, not just the library: scripts/test-on-android.sh runs
# them on the device, and without them it silently skips the half of its work
# that needs them, which reads as a pass.
cmake --build "$HERE/build-android" -j"$(nproc)" \
      --target uninet_c test_roundtrip test_network test_cabi uninet_demo

OUT="$HERE/build-android/libuninet_c.so"
echo
file "$OUT"
READELF="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf"
echo "depends on:"
"$READELF" -d "$OUT" | sed -n 's/.*Shared library: \[\(.*\)\]/  \1/p'

# Asserted, not assumed: see note 3. A build that quietly loses LZ4 still links,
# still passes every on-device test, still discovers peers, and then drops every
# message a desktop peer sends it. Catch it here instead.
NM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
if [ "$("$NM" "$OUT" 2>/dev/null | grep -c 'LZ4F_')" -eq 0 ]; then
    echo
    echo "FAILED: this build has no LZ4 support." >&2
    echo "It would silently discard every message from a peer that has it." >&2
    exit 1
fi
echo "  LZ4 tier: present (can decode frames from any other UniNet build)"
echo
echo "Copy it into your Unity project:"
echo "  cp $OUT <UnityProject>/Assets/Plugins/Android/libs/$ABI/"
echo
echo "Android also silently drops multicast unless the app holds a Wi-Fi"
echo "MulticastLock. See docs/unity/UniNetMulticastLock.cs and the manifest"
echo "permission CHANGE_WIFI_MULTICAST_STATE, or discovery will find nothing."
