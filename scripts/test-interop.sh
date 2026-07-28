#!/usr/bin/env bash
# UniNet cross-language interop test.
#
#     ./scripts/test-interop.sh [seconds]      # how long to run, default 25
#
# Starts a C++, a Python and a C# node in one private realm. Each publishes an
# identical payload and verifies what the others send. It passes only if every
# participant saw both of the others AND every field decoded to exactly the
# expected value, which is what "the same data in every language" has to mean.
#
# C# is skipped with a clear notice when the .NET SDK is absent; C++ and Python
# still run, and the script reports which languages were actually covered.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${UNINET_BUILD_DIR:-$HERE/build}"
REALM="uninet-interop-$$"
SECONDS_LIMIT="${1:-25}"
LOGDIR="$(mktemp -d)"
PIDS=()
COVERED=()
SKIPPED=()

# Decide up front which languages will take part, so each participant can be
# told exactly whom to expect. Counting an uninstalled runtime as a missing peer
# would turn "not covered" into "broken", which are very different results.
have_python=0
have_csharp=0
PYTHON="${PYTHON:-python3}"
PY_ENV=()
# The in-tree build FIRST. Preferring whatever is on the path let a stale
# `pip install .` in site-packages shadow the build tree, so this test could
# validate an old binding against a freshly built core and still report PASS,
# which is the exact drift it exists to catch.
if PYTHONPATH="$HERE/python:${PYTHONPATH:-}" "$PYTHON" -c 'import uninet' >/dev/null 2>&1; then
    have_python=1
    PY_ENV=(env "PYTHONPATH=$HERE/python:${PYTHONPATH:-}")
elif "$PYTHON" -c 'import uninet' >/dev/null 2>&1; then
    have_python=1
fi
if command -v dotnet >/dev/null 2>&1 && \
   { [ -f "$BUILD/libuninet_c.so" ] || [ -f "$BUILD/uninet_c.dll" ] || [ -f "$BUILD/libuninet_c.dylib" ]; }; then
    have_csharp=1
fi

peers_excluding() {   # peers_excluding <self> -> csv of the other participants
    local self="$1" out=()
    [ "$self" != "cpp" ] && out+=("cpp")
    [ "$self" != "python" ] && [ "$have_python" -eq 1 ] && out+=("python")
    [ "$self" != "csharp" ] && [ "$have_csharp" -eq 1 ] && out+=("csharp")
    local IFS=,; echo "${out[*]}"
}

cleanup() {
    for pid in "${PIDS[@]:-}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    # Also on an early exit: the "interop_cpp not found" path and Ctrl-C both
    # used to leave the log directory behind.
    rm -rf "$LOGDIR"
}
trap cleanup EXIT INT TERM

echo "UniNet cross-language interop"
echo "realm: $REALM"
echo

# ── C++ ───────────────────────────────────────────────────────────────────
if [ ! -x "$BUILD/interop_cpp" ]; then
    echo "ERROR: $BUILD/interop_cpp not found. Build first:" >&2
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
    exit 1
fi
"$BUILD/interop_cpp" "$REALM" "$SECONDS_LIMIT" "$(peers_excluding cpp)" > "$LOGDIR/cpp.log" 2>&1 &
PIDS+=($!)
COVERED+=("C++")

# ── Python ────────────────────────────────────────────────────────────────
if [ "$have_python" -eq 0 ]; then
    SKIPPED+=("Python (module not importable: pip install . or build with -DUNINET_BUILD_PYTHON=ON)")
else
    "${PY_ENV[@]}" "$PYTHON" "$HERE/tests/interop/interop_py.py" \
        "$REALM" "$SECONDS_LIMIT" "$(peers_excluding python)" > "$LOGDIR/python.log" 2>&1 &
    PIDS+=($!)
    COVERED+=("Python")
fi

# ── C# ────────────────────────────────────────────────────────────────────
if [ "$have_csharp" -eq 0 ]; then
    if command -v dotnet >/dev/null 2>&1; then
        SKIPPED+=("C# (native library missing: configure with -DUNINET_BUILD_CABI=ON)")
    else
        SKIPPED+=("C# (dotnet not installed)")
    fi
else
    # The C# side finds the native library through the loader path.
    LD_LIBRARY_PATH="$BUILD:${LD_LIBRARY_PATH:-}" \
    DYLD_LIBRARY_PATH="$BUILD:${DYLD_LIBRARY_PATH:-}" \
    dotnet run --project "$HERE/tests/interop/InteropCs" -c Release \
        -- "$REALM" "$SECONDS_LIMIT" "$(peers_excluding csharp)" > "$LOGDIR/csharp.log" 2>&1 &
    PIDS+=($!)
    COVERED+=("C#")
fi

# ── wait for all participants ─────────────────────────────────────────────
FAILED=0
for pid in "${PIDS[@]}"; do
    wait "$pid" || FAILED=1
done
PIDS=()

for f in "$LOGDIR"/*.log; do
    [ -e "$f" ] || continue
    echo "──────── $(basename "$f" .log) ────────"
    cat "$f"
    echo
done
rm -rf "$LOGDIR"

echo "languages covered: ${COVERED[*]}"
# Machine readable, so a caller can tell a full run from a partial one.
echo "INTEROP_COVERED=${COVERED[*]}"
for s in "${SKIPPED[@]:-}"; do [ -n "$s" ] && echo "SKIPPED: $s"; done

# Two participants can trivially "pass" by both being wrong in the same way, so
# a run that covered fewer than two languages proves nothing.
if [ "${#COVERED[@]}" -lt 2 ]; then
    echo
    echo "INCONCLUSIVE: interop needs at least two languages."
    exit 1
fi

echo
if [ "$FAILED" -eq 0 ]; then
    echo "INTEROP PASS, every language decoded every other language's payload identically."
else
    echo "INTEROP FAIL (see the logs above)."
fi
exit "$FAILED"
