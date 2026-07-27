#!/usr/bin/env bash
# Compile every UniNet translation unit for Windows and fail on any diagnostic.
set -uo pipefail

CXX=x86_64-w64-mingw32-g++-posix
CC=x86_64-w64-mingw32-gcc-posix
echo "=== UniNet Windows portability check ==="
$CXX --version | head -1
echo

cp -r /src /work && cd /work
mkdir -p /tmp/obj

# -D_WIN32_WINNT: czmq/zyre headers gate Windows API levels on it, exactly as a
# real MSVC build would via its project settings.
FLAGS=(-std=c++17 -Wall -Wextra -Werror -c -O1
       -D_WIN32_WINNT=0x0601 -DUNINET_HAS_ZLIB=1
       -Iinclude -I/hdr/zyre/include -I/hdr/czmq/include -I/hdr/libzmq/include)

FAILED=0
for f in src/*.cpp; do
    out="/tmp/obj/$(basename "$f" .cpp).obj"
    if ! $CXX "${FLAGS[@]}" "$f" -o "$out" 2>/tmp/err.txt; then
        echo "FAIL  $f"
        sed 's/^/      /' /tmp/err.txt | head -20
        FAILED=1
    else
        if [ -s /tmp/err.txt ]; then
            echo "WARN  $f"; sed 's/^/      /' /tmp/err.txt | head -10; FAILED=1
        else
            echo "ok    $f"
        fi
    fi
done

# The C ABI must also compile as C for a consumer that includes it from C.
if ! $CC -std=c99 -Wall -Wextra -Werror -c -O1 -D_WIN32_WINNT=0x0601 \
        -Iinclude -I/hdr/zyre/include -I/hdr/czmq/include -I/hdr/libzmq/include tests/test_cabi.c -o /tmp/obj/test_cabi.obj 2>/tmp/err.txt; then
    echo "FAIL  tests/test_cabi.c (as C)"
    sed 's/^/      /' /tmp/err.txt | head -20
    FAILED=1
else
    echo "ok    tests/test_cabi.c (as C)"
fi

echo
echo "=== RESULT: $([ $FAILED -eq 0 ] && echo 'PASS (all sources compile for Windows)' || echo FAIL) ==="
exit $FAILED
