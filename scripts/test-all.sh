#!/usr/bin/env bash
# UniNet — run every test we can run on this machine.
#
#     ./scripts/test-all.sh              native suites only (fast)
#     ./scripts/test-all.sh --docker     also the containerised cross-platform
#                                        and cross-language suites
#
# Native covers: C++ core, network, the C ABI compiled as C, and Python.
# --docker adds: a Debian build with Zyre compiled from source (the path a
# machine without a system Zyre takes) running all three languages; a MinGW
# compile check for Windows; and a Wine run of the cross-compiled Windows
# binaries. See tests/docker/Dockerfile.windows-run for exactly how far the
# Windows coverage goes — the network layer needs a real Windows machine.
#
# Every stage reports PASS/SKIP/FAIL and the script exits non-zero if any FAILed.
# A SKIP is never counted as a pass — an untested thing is not a working thing.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"
BUILD="${UNINET_BUILD_DIR:-$HERE/build}"
USE_DOCKER=0
[ "${1:-}" = "--docker" ] && USE_DOCKER=1

PASSED=(); SKIPPED=(); FAILED=()

stage() { printf '\n\033[1m── %s ──\033[0m\n' "$1"; }
pass()  { PASSED+=("$1");  echo "PASS  $1"; }
skip()  { SKIPPED+=("$1"); echo "SKIP  $1 — $2"; }
fail()  { FAILED+=("$1");  echo "FAIL  $1"; }

# ── native ────────────────────────────────────────────────────────────────
stage "build"
BUILD_LOG="$(mktemp)"
if ! cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DUNINET_BUILD_CABI=ON \
        > "$BUILD_LOG" 2>&1; then
    tail -15 "$BUILD_LOG"
    fail "cmake configure"
elif ! cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)" \
        >> "$BUILD_LOG" 2>&1; then
    grep -E 'error' "$BUILD_LOG" | head -15
    fail "build"
else
    # `grep -c` on a pipeline can emit a trailing newline; tr keeps it one token.
    WARN="$(grep -c 'warning' "$BUILD_LOG" | tr -d '\n ')"
    grep 'warning' "$BUILD_LOG" | head -10
    pass "build (${WARN:-0} warnings)"
fi
rm -f "$BUILD_LOG"

stage "C++ and C ABI (ctest)"
if ctest --test-dir "$BUILD" --output-on-failure 2>&1 | tail -6; then
    pass "ctest"
else
    fail "ctest"
fi

stage "Python"
if PYTHONPATH="$HERE/python" python3 -c 'import uninet' >/dev/null 2>&1; then
    if PYTHONPATH="$HERE/python" python3 -m pytest python/tests -q 2>&1 | tail -3; then
        pass "pytest"
    else
        fail "pytest"
    fi
else
    skip "pytest" "the uninet module is not built (configure with -DUNINET_BUILD_PYTHON=ON)"
fi

stage "cross-language interop"
if OUT="$(./scripts/test-interop.sh 25 2>&1)"; then
    echo "$OUT" | tail -8
    pass "interop ($(echo "$OUT" | grep 'languages covered' | cut -d: -f2-))"
else
    echo "$OUT" | tail -14
    fail "interop"
fi

# ── containerised ─────────────────────────────────────────────────────────
if [ "$USE_DOCKER" -eq 1 ]; then
    if ! docker info >/dev/null 2>&1; then
        skip "docker suites" "docker is not usable by this user"
    else
        # The host resolver is often 127.0.0.53 (systemd-resolved), which a
        # container cannot reach; --network host sidesteps it for the build.
        stage "Linux container (Zyre from source, C++/Python/C#)"
        if docker build --network host -f tests/docker/Dockerfile.linux \
                -t uninet-test . >/dev/null 2>&1 \
           && docker run --rm --network host -v "$PWD":/src:ro uninet-test 2>&1 | tail -25; then
            pass "linux container"
        else
            fail "linux container"
        fi

        stage "Windows compile check (MinGW)"
        if docker build --network host -f tests/docker/Dockerfile.windows-check \
                -t uninet-wincheck . >/dev/null 2>&1 \
           && docker run --rm -v "$PWD":/src:ro uninet-wincheck 2>&1 | tail -16; then
            pass "windows compile"
        else
            fail "windows compile"
        fi

        stage "Windows runtime (MinGW + Wine)"
        # Runs the real .exe files. Covers the codec, JSON and C ABI on Windows;
        # discovery stops at a Wine limitation, which the runner treats as an
        # expected stop rather than a pass. See the Dockerfile for why.
        if docker build --network host -f tests/docker/Dockerfile.windows-run \
                -t uninet-winrun . >/dev/null 2>&1 \
           && docker run --rm --network host -v "$PWD":/src:ro uninet-winrun 2>&1 | tail -20; then
            pass "windows runtime (network layer not covered — needs real Windows)"
        else
            fail "windows runtime"
        fi
    fi
else
    skip "docker suites" "not requested (pass --docker)"
fi

# ── verdict ───────────────────────────────────────────────────────────────
stage "summary"
for s in "${PASSED[@]:-}";  do [ -n "$s" ] && echo "  PASS  $s"; done
for s in "${SKIPPED[@]:-}"; do [ -n "$s" ] && echo "  SKIP  $s"; done
for s in "${FAILED[@]:-}";  do [ -n "$s" ] && echo "  FAIL  $s"; done
echo
if [ "${#FAILED[@]}" -gt 0 ]; then
    echo "RESULT: FAIL (${#FAILED[@]} stage(s))"
    exit 1
fi
echo "RESULT: PASS"
[ "${#SKIPPED[@]}" -gt 0 ] && echo "note: ${#SKIPPED[@]} stage(s) were skipped, not verified."
exit 0
