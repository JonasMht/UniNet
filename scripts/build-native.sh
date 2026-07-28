#!/usr/bin/env bash
# Build the native library that C#, Unity and any other FFI consumer loads, in
# the form you can hand to somebody else, and say where to put it.
#
#     ./scripts/build-native.sh                 # build, then print the copy targets
#     ./scripts/build-native.sh --into <dir>    # also copy it there
#     ./scripts/build-native.sh --system-deps   # link the machine's ZeroMQ instead
#
# The default build compiles ZeroMQ, czmq, zyre and lz4 into the library, which
# takes a few minutes the first time. That is the point of this script: a library
# linked against the ZeroMQ *installed on the build machine* loads only on
# machines that have the same one, and the failure lands on whoever you gave it
# to, at startup, naming a library they never installed. --system-deps gives the
# faster, machine-specific build for local development.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="$ROOT/build-native"
INTO=""
SELF_CONTAINED=ON

while [ $# -gt 0 ]; do
    case "$1" in
        --into)        INTO="${2:-}"; shift 2 ;;
        --system-deps) SELF_CONTAINED=OFF; shift ;;
        -h|--help)
            sed -n '2,20p' "${BASH_SOURCE[0]}" | sed -n 's/^# \?//p'
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

say() { printf '\033[1;34m▶ %s\033[0m\n' "$*"; }
ok()  { printf '\033[1;32m✓ %s\033[0m\n' "$*"; }

say "Configuring (self-contained dependencies: $SELF_CONTAINED)"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DUNINET_BUILD_CABI=ON -DUNINET_BUILD_PYTHON=OFF \
      -DUNINET_SELF_CONTAINED=$SELF_CONTAINED >/dev/null

say "Building"
cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)" \
      --target uninet_c >/dev/null

# Not `ls a b c | head -1`: under `set -o pipefail` that exits 2 as soon as one
# of the names does not exist, which on any single platform is most of them.
LIB=""
for candidate in "$BUILD_DIR/libuninet_c.so" "$BUILD_DIR/libuninet_c.dylib" \
                 "$BUILD_DIR/uninet_c.dll" "$BUILD_DIR/Release/uninet_c.dll"; do
    [ -f "$candidate" ] && { LIB="$candidate"; break; }
done
[ -n "$LIB" ] || { echo "the build produced no library" >&2; exit 1; }
ok "built $LIB"

# What else must travel with it. On a self-contained build this should list only
# the C++ runtime and libc; anything else is a file the other machine also needs.
if command -v ldd >/dev/null && [ "${LIB##*.}" = "so" ]; then
    EXTRA="$(ldd "$LIB" | awk '{print $1}' \
             | grep -vE '^(linux-vdso|libstdc\+\+|libgcc_s|libc|libm|libdl|libpthread|librt|/lib64/ld-linux)' \
             | grep -v '^$' || true)"
elif command -v otool >/dev/null; then
    EXTRA="$(otool -L "$LIB" | tail -n +2 | awk '{print $1}' \
             | grep -vE '(libc\+\+|libSystem)' || true)"
else
    EXTRA=""
fi
if [ -n "$EXTRA" ]; then
    printf '\033[1;33m! it also needs, on every machine you copy it to:\033[0m\n'
    printf '    %s\n' $EXTRA
    echo "  (a --system-deps build links the machine's own ZeroMQ; the default does not)"
else
    ok "self-contained: nothing beyond the C++ runtime has to travel with it"
fi

if [ -n "$INTO" ]; then
    mkdir -p "$INTO"
    cp "$LIB" "$INTO/"
    ok "copied to $INTO/$(basename "$LIB")"
fi

cat <<EOF

Where it goes
─────────────
  .NET project          nothing to do if it references csharp/UniNet/UniNet.csproj:
                        the project copies $(basename "$LIB") next to your executable.
                        Otherwise put it in the output directory (bin/<config>/<tfm>/).
  published .NET app    next to the .exe / the app dll.
  Unity, desktop        Assets/Plugins/x86_64/$(basename "$LIB")
                        (Inspector: Standalone + the matching OS, CPU x86_64.
                         Restart the Editor after replacing it - it does not
                         reload a native plugin that is already loaded.)
  Unity, Quest/Android  Assets/Plugins/Android/libs/arm64-v8a/libuninet_c.so
                        built by ./scripts/build-for-android.sh, not by this script.
  anything else         a directory on LD_LIBRARY_PATH (Linux),
                        DYLD_LIBRARY_PATH (macOS) or PATH (Windows).
EOF
