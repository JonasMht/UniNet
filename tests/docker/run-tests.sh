#!/usr/bin/env bash
# Build UniNet from the mounted source and run every suite: C++, the C ABI
# compiled as C, Python, and the three-language interop test.
#
# The source is mounted read-only at /src and copied, so a container run never
# writes into the developer's tree.
set -uo pipefail

echo "=== UniNet test container ==="
cmake --version | head -1
python3 --version
dotnet --version
pkg-config --modversion libzyre 2>/dev/null | sed 's/^/zyre /'
echo

cp -r /src /work && cd /work && rm -rf build
FAILED=0

echo "=== configure + build ==="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DUNINET_BUILD_CABI=ON -DUNINET_BUILD_PYTHON=ON >/dev/null || FAILED=1
BUILD_OUT="$(cmake --build build -j"$(nproc)" 2>&1)"
BUILD_RC=$?
grep -E 'warning|error' <<<"$BUILD_OUT" | head -20
# grep -q inside a pipeline is a trap under `set -o pipefail`: grep exits on the
# first match, the producer takes SIGPIPE, and the pipeline reports failure — so
# `if ... | grep -q error` reads as "no error". Use a herestring instead.
if [ $BUILD_RC -ne 0 ] || grep -q 'error' <<<"$BUILD_OUT"; then
    echo "BUILD FAILED"; exit 1
fi
WARNINGS="$(grep -c 'warning' <<<"$BUILD_OUT" | tr -d '\n ')"
echo "build OK (${WARNINGS} warnings)"
echo

echo "=== ctest (C++ core, network, C ABI) ==="
ctest --test-dir build --output-on-failure 2>&1 | tail -12 || FAILED=1
echo

echo "=== python ==="
export PYTHONPATH=/work/python
python3 -m pytest python/tests -q 2>&1 | tail -6 || FAILED=1
echo

echo "=== cross-language interop (C++ / Python / C#) ==="
./scripts/test-interop.sh 25 2>&1 | tail -20 || FAILED=1
echo

echo "=== RESULT: $([ $FAILED -eq 0 ] && echo PASS || echo FAIL) ==="
exit $FAILED
