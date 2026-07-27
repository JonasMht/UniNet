#!/usr/bin/env bash
# Cross-compile UniNet for Windows, then run its test binaries under Wine.
# See Dockerfile.windows-run for what this does and does not prove.
set -uo pipefail

echo "=== UniNet Windows runtime test ==="
x86_64-w64-mingw32-g++-posix --version | head -1
wine --version
echo

cp -r /src /work && cd /work && rm -rf build-win
export PKG_CONFIG_LIBDIR=/usr/x86_64-w64-mingw32/lib/pkgconfig
export PKG_CONFIG_PATH=$PKG_CONFIG_LIBDIR

echo "=== configure ==="
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
      -DUNINET_BUILD_CABI=ON -DUNINET_ZMQ_STATIC=ON -DUNINET_SYSTEM_ZYRE=ON \
      2>&1 | grep -Ei 'zyre|lz4|error' | head -6
echo

echo "=== build ==="
OUT="$(cmake --build build-win -j"$(nproc)" 2>&1)"; RC=$?
grep -E 'error' <<<"$OUT" | head -20
# `grep -q` inside a pipeline is a trap under `set -o pipefail`: grep exits on
# the first match, the producer takes SIGPIPE, and the pipeline reports failure,
# so `if ... | grep -q error` reads as "no error". Use a herestring.
if [ $RC -ne 0 ] || grep -q 'error' <<<"$OUT"; then
    echo "BUILD FAILED"; exit 1
fi
echo "build OK — $(ls build-win/*.exe 2>/dev/null | wc -l) Windows executables linked"
echo

# MinGW links its C++ runtime dynamically and zlib was cross-built as a DLL, so
# the .exe files need those beside them — exactly as a Windows deployment would
# ship them. Resolve from the import table so a new dependency is never missed.
for dll in $(x86_64-w64-mingw32-objdump -p build-win/test_roundtrip.exe \
             | awk '/DLL Name:/ {print $3}' | sort -u); do
    case "$dll" in
        KERNEL32.dll|msvcrt.dll|ADVAPI32.dll|WS2_32.dll|IPHLPAPI.DLL|RPCRT4.dll) continue ;;
    esac
    src="$(find /usr/lib/gcc/x86_64-w64-mingw32 /usr/x86_64-w64-mingw32 -name "$dll" 2>/dev/null | head -1)"
    [ -n "$src" ] && cp "$src" build-win/
done

FAILED=0

echo "=== test_roundtrip.exe — codec, compression, framing, hostile input ==="
TOUT="$(wine build-win/test_roundtrip.exe 2>&1)"; TRC=$?
tail -5 <<<"$TOUT"
if [ $TRC -eq 0 ]; then echo "  -> PASS"; else echo "  -> FAIL (exit $TRC)"; FAILED=1; fi
echo

# These reach the network and stop at Wine's GetAdaptersAddresses limitation.
# Their pre-network sections are still real Windows coverage, so they are run
# and reported — but a stop at ziflist.c is expected, not a regression.
for t in test_cabi test_network; do
    echo "=== $t.exe — up to the network boundary ==="
    TOUT="$(wine "build-win/$t.exe" 2>&1)"; TRC=$?
    tail -6 <<<"$TOUT"
    if [ $TRC -eq 0 ]; then
        echo "  -> PASS (Wine handled the network too)"
    elif grep -q 'ziflist.c' <<<"$TOUT"; then
        echo "  -> EXPECTED STOP: Wine does not implement GetAdaptersAddresses'"
        echo "     buffer-sizing protocol (czmq ziflist.c). Pre-network checks passed."
    else
        echo "  -> FAIL (exit $TRC) — not the known Wine limitation"; FAILED=1
    fi
    echo
done

echo "=== RESULT: $([ $FAILED -eq 0 ] && echo PASS || echo FAIL) ==="
echo "note: discovery/messaging on Windows is NOT covered here — see the"
echo "      windows:msvc job in .gitlab-ci.yml, which needs a Windows runner."
exit $FAILED
