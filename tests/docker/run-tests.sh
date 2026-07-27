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
# first match, the producer takes SIGPIPE, and the pipeline reports failure: so
# `if ... | grep -q error` reads as "no error". Use a herestring instead.
if [ $BUILD_RC -ne 0 ] || grep -q 'error' <<<"$BUILD_OUT"; then
    echo "BUILD FAILED"; exit 1
fi
WARNINGS="$(grep -c 'warning' <<<"$BUILD_OUT" | tr -d '\n ')"
echo "build OK (${WARNINGS} warnings)"
echo

echo "=== ctest (C++ core, network, C ABI) ==="
ctest --test-dir build -L uninet --no-tests=error --output-on-failure 2>&1 | tail -12 || FAILED=1
echo

echo "=== python ==="
export PYTHONPATH=/work/python
python3 -m pytest python/tests -q 2>&1 | tail -6 || FAILED=1
echo

echo "=== cross-language interop (C++ / Python / C#) ==="
./scripts/test-interop.sh 25 2>&1 | tail -20 || FAILED=1
echo

# The wheel is a different artifact from the in-tree build, and only this checks
# it. `pip install .` once shipped a package with __init__.py and no extension,
# because the extension had a build-tree output directory and no install rule.
# Every in-tree test still passed, since they import from python/ where the
# build had already put the file. So: install it, then import it from a
# directory where the source tree cannot possibly shadow it.
echo "=== pip wheel, imported from outside the source tree ==="
# Output kept, not discarded. Hiding it here cost a debugging round: the venv
# step failed because the image lacked python3-venv, and all the suite could
# say was "WHEEL BUILD FAILED".
WHEEL_LOG=/tmp/wheel-build.log
if python3 -m venv /tmp/wheelenv > "$WHEEL_LOG" 2>&1 && \
   /tmp/wheelenv/bin/pip install . >> "$WHEEL_LOG" 2>&1; then
    if (cd /tmp && env -u PYTHONPATH /tmp/wheelenv/bin/python -c "
import uninet, os
from uninet import _uninet
assert 'site-packages' in uninet.__file__, uninet.__file__
print('wheel OK:', uninet.__version__, os.path.basename(_uninet.__file__))
"); then :; else echo "WHEEL IMPORT FAILED"; FAILED=1; fi
else
    echo "WHEEL BUILD FAILED:"
    tail -25 "$WHEEL_LOG"
    FAILED=1
fi
echo

echo "=== RESULT: $([ $FAILED -eq 0 ] && echo PASS || echo FAIL) ==="
exit $FAILED
