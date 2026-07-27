#!/usr/bin/env bash
# UniNet: run every test we can run on this machine.
#
#     ./scripts/test-all.sh              native suites only (fast)
#     ./scripts/test-all.sh --docker     also the containerised cross-platform
#                                        and cross-language suites
#     ./scripts/test-all.sh --sanitizers also ThreadSanitizer and ASan/UBSan
#
# Native covers: C++ core, network, the C ABI compiled as C, and Python.
# --docker adds: a Debian build with Zyre compiled from source (the path a
# machine without a system Zyre takes) running all three languages; a MinGW
# compile check for Windows; and a Wine run of the cross-compiled Windows
# binaries. See tests/docker/Dockerfile.windows-run for exactly how far the
# Windows coverage goes: the network layer needs a real Windows machine.
#
# Every stage reports PASS/SKIP/FAIL and the script exits non-zero if any FAILed.
# A SKIP is never counted as a pass, an untested thing is not a working thing.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"
BUILD="${UNINET_BUILD_DIR:-$HERE/build}"
USE_DOCKER=0
USE_SAN=0
for arg in "$@"; do
    case "$arg" in
        --docker)     USE_DOCKER=1 ;;
        --sanitizers) USE_SAN=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

PASSED=(); SKIPPED=(); FAILED=()

stage() { printf '\n\033[1m── %s ──\033[0m\n' "$1"; }
pass()  { PASSED+=("$1");  echo "PASS  $1"; }
skip()  { SKIPPED+=("$1"); echo "SKIP  $1: $2"; }
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
# --no-tests=error: `ctest -L uninet` exits 0 when the label matches NOTHING,
# so a renamed label or a forgotten LABELS property would report a green
# "PASS ctest" having run zero tests. The count is asserted too, because
# --no-tests=error only catches the zero case, not "one of three vanished".
CTEST_N="$(ctest --test-dir "$BUILD" -L uninet -N 2>/dev/null | sed -n 's/^Total Tests: //p')"
if [ "${CTEST_N:-0}" -lt 3 ]; then
    fail "ctest (only ${CTEST_N:-0} tests matched -L uninet; expected at least 3)"
elif ctest --test-dir "$BUILD" -L uninet --no-tests=error --output-on-failure 2>&1 | tail -6; then
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
    COVERED_LANGS="$(grep '^INTEROP_COVERED=' <<<"$OUT" | cut -d= -f2-)"
    pass "interop (${COVERED_LANGS:-unknown})"
    # A partial run is not a full pass. Record the gap so the summary's skip
    # count is truthful rather than implying all three languages were checked.
    for lang in "C++" "Python" "C#"; do
        case " $COVERED_LANGS " in
            *" $lang "*) ;;
            *) skip "interop $lang" "that runtime was not available" ;;
        esac
    done
else
    echo "$OUT" | tail -14
    fail "interop"
fi

# ── sanitizers ────────────────────────────────────────────────────────────
if [ "$USE_SAN" -eq 1 ]; then
    stage "ThreadSanitizer (whole stack, dependencies included)"
    # The dependencies MUST be instrumented too. Against a system libzmq that
    # TSan cannot see into, it reported 47 races, all but one of them noise from
    # synchronisation it could not observe. Built from source under TSan, the
    # same run reports zero.
    SAN_TSAN="$HERE/build-tsan"
    if cmake -S . -B "$SAN_TSAN" -DCMAKE_BUILD_TYPE=Debug -DUNINET_SYSTEM_ZYRE=OFF \
            -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1" \
            -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
            -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" >/dev/null 2>&1 \
       && cmake --build "$SAN_TSAN" -j"$(nproc 2>/dev/null || echo 4)" \
               --target test_network test_reconnect >/dev/null 2>&1; then
        # setarch -R: TSan aborts with "unexpected memory mapping" under ASLR
        # often enough to make the stage flaky otherwise.
        # The reconnect test too: it is the most concurrent thing in the tree,
        # with a watchdog thread rebuilding the node under the actor thread
        # while callers read the identity. Both races found during its
        # development were here and nowhere else.
        setarch -R "$SAN_TSAN/test_network" > /tmp/uninet-tsan.log 2>&1; NET_RC=$?
        "$HERE/scripts/test-reconnect.sh" "$SAN_TSAN/test_reconnect" \
            >> /tmp/uninet-tsan.log 2>&1; REC_RC=$?
        if [ $NET_RC -eq 0 ] && [ $REC_RC -eq 0 ]; then
            RACES="$(grep -c 'WARNING: ThreadSanitizer' /tmp/uninet-tsan.log || true)"
            if [ "${RACES:-0}" -eq 0 ]; then pass "tsan (0 races)"; else
                grep -A6 'WARNING: ThreadSanitizer' /tmp/uninet-tsan.log | head -20
                fail "tsan ($RACES races)"
            fi
        else
            RACES="$(grep -c 'WARNING: ThreadSanitizer' /tmp/uninet-tsan.log || true)"
            grep -A6 'WARNING: ThreadSanitizer' /tmp/uninet-tsan.log | head -20
            fail "tsan ($RACES races)"
        fi
    else
        fail "tsan (build)"
    fi

    stage "AddressSanitizer + UndefinedBehaviorSanitizer"
    SAN_ASAN="$HERE/build-asan"
    if cmake -S . -B "$SAN_ASAN" -DCMAKE_BUILD_TYPE=Debug -DUNINET_BUILD_CABI=ON \
            -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
            -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" >/dev/null 2>&1 \
       && cmake --build "$SAN_ASAN" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null 2>&1; then
        SAN_FAIL=0
        ASAN_OPTIONS=detect_leaks=1 "$SAN_ASAN/test_roundtrip" >/tmp/uninet-asan.log 2>&1 || SAN_FAIL=1
        ASAN_OPTIONS=detect_leaks=0 "$SAN_ASAN/test_network" >>/tmp/uninet-asan.log 2>&1 || SAN_FAIL=1
        if grep -qE 'ERROR: (Address|Leak)Sanitizer|runtime error' /tmp/uninet-asan.log; then SAN_FAIL=1; fi
        if [ "$SAN_FAIL" -eq 0 ]; then pass "asan/ubsan"; else
            grep -E 'ERROR|runtime error' /tmp/uninet-asan.log | head -10
            fail "asan/ubsan"
        fi
    else
        fail "asan/ubsan (build)"
    fi
else
    skip "sanitizers" "not requested (pass --sanitizers)"
fi

# ── containerised ─────────────────────────────────────────────────────────
if [ "$USE_DOCKER" -eq 1 ]; then
    if ! docker info >/dev/null 2>&1; then
        skip "docker suites" "docker is not usable by this user"
    else
        # The host resolver is often 127.0.0.53 (systemd-resolved), which a
        # container cannot reach; --network host sidesteps it for the build.
        # Build and run are reported separately on purpose. Folded into one
        # `docker build ... && docker run ...` with the build output discarded,
        # a broken base image or an apt mirror having a bad day reads as "the
        # tests failed", and the actual reason is nowhere on screen.
        docker_image() {   # docker_image <tag> <dockerfile>
            local out
            if ! out="$(docker build --network host -f "$2" -t "$1" . 2>&1)"; then
                echo "could not build the $1 image:"
                tail -15 <<<"$out"
                return 1
            fi
            return 0
        }

        stage "Linux container (Zyre from source, C++/Python/C#)"
        if docker_image uninet-test tests/docker/Dockerfile.linux \
           && docker run --rm --network host -v "$PWD":/src:ro uninet-test 2>&1 | tail -25; then
            pass "linux container"
        else
            fail "linux container"
        fi

        stage "Windows compile check (MinGW)"
        if docker_image uninet-wincheck tests/docker/Dockerfile.windows-check \
           && docker run --rm -v "$PWD":/src:ro uninet-wincheck 2>&1 | tail -16; then
            pass "windows compile"
        else
            fail "windows compile"
        fi

        stage "Windows runtime (MinGW + Wine)"
        # Runs the real .exe files. Covers the codec, JSON and C ABI on Windows;
        # discovery stops at a Wine limitation, which the runner treats as an
        # expected stop rather than a pass. See the Dockerfile for why.
        if docker_image uninet-winrun tests/docker/Dockerfile.windows-run \
           && docker run --rm --network host -v "$PWD":/src:ro uninet-winrun 2>&1 | tail -20; then
            pass "windows runtime (network layer not covered: needs real Windows)"
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
