#!/usr/bin/env bash
# End-to-end test of scripts/UniNetSlicer.py against a real 3D Slicer.
#
#     ./scripts/test-slicer-setup.sh [/path/to/Slicer-5.8.1-linux-amd64]
#
# The unit tests (python/tests/test_slicer_setup.py) cover every decision the
# installer makes. This covers the part they cannot: that the thing it builds
# loads into Slicer's Python and talks to another copy of itself.
#
# It leaves the Slicer it was given with UniNet installed, which is the state
# anyone running this wanted anyway. It uses its own cache directory, so a
# developer's real one is neither read nor overwritten, and every UniNet session
# it starts is in its own realm, so a demo running on the same machine cannot
# change the result.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SETUP="$HERE/scripts/UniNetSlicer.py"
SLICER="${1:-}"

PASS=0
FAIL=0
pass() { printf '\033[1;32m✓\033[0m %s\n' "$*"; PASS=$((PASS + 1)); }
fail() { printf '\033[1;31m✗\033[0m %s\n' "$*"; FAIL=$((FAIL + 1)); }
step() { printf '\n\033[1;34m▶ %s\033[0m\n' "$*"; }

PYTHON="$(command -v python3 || command -v python)"
[ -n "$PYTHON" ] || { echo "no python3 on PATH"; exit 2; }

# Find Slicer the same way the installer does, so this script needs no argument
# either, and report what was chosen: testing the wrong installation silently is
# the failure mode worth avoiding here.
if [ -z "$SLICER" ]; then
    SLICER="$("$PYTHON" "$SETUP" status 2>/dev/null | awk '$1 == "slicer" {print $3}')"
fi
[ -n "$SLICER" ] && [ -x "$SLICER/bin/PythonSlicer" ] || {
    echo "usage: $0 /path/to/Slicer-X.Y.Z-...   (no Slicer was found automatically)" >&2
    exit 2
}
PYSLICER="$SLICER/bin/PythonSlicer"
echo "Slicer   : $SLICER"
echo "installer: $SETUP"

export UNINET_CACHE="${UNINET_CACHE:-${TMPDIR:-/tmp}/uninet-slicer-test-cache}"
echo "cache    : $UNINET_CACHE"
REALM="uninet-slicer-test-$$"

# ── 1. from nothing ──────────────────────────────────────────────────────────
step "Removing any existing install, then installing from scratch"
"$PYTHON" "$SETUP" uninstall --slicer "$SLICER" >/dev/null 2>&1
if "$PYSLICER" -c "import uninet" >/dev/null 2>&1; then
    fail "uninstall left something importable behind"
else
    pass "uninstall leaves nothing importable"
fi

START=$(date +%s)
if "$PYTHON" "$SETUP" install --slicer "$SLICER" >/tmp/uninet-slicer-install.log 2>&1; then
    pass "install ($(($(date +%s) - START))s, log in /tmp/uninet-slicer-install.log)"
else
    fail "install"
    tail -20 /tmp/uninet-slicer-install.log
    exit 1
fi

# ── 2. what got installed ────────────────────────────────────────────────────
step "Checking what Slicer's Python can now do"
if "$PYSLICER" -c "
import uninet, sys
assert uninet.HAS_LZ4, 'built without the lz4 tier: it would silently drop traffic from peers that have it'
print('   ', uninet.__version__, uninet.zyre_version(), 'lz4', uninet.HAS_LZ4)
"; then
    pass "import uninet, with both compression tiers"
else
    fail "import uninet"
fi

if [ "$(uname)" = "Linux" ]; then
    EXT="$("$PYSLICER" -c "import uninet, pathlib; print(next(pathlib.Path(uninet.__file__).parent.glob('_uninet*.so')))")"
    if ldd "$EXT" | grep -qE "libzyre|libczmq|libzmq"; then
        fail "the extension needs a system ZeroMQ, so the wheel is not portable"
        ldd "$EXT" | grep -E "libzyre|libczmq|libzmq"
    else
        pass "the extension is self-contained (no libzyre/libczmq/libzmq needed)"
    fi
fi

# ── 3. two of them, talking ──────────────────────────────────────────────────
step "Two Slicer interpreters discovering each other and exchanging a message"
cat > /tmp/uninet-slicer-peer.py <<'PY'
import sys, time, uninet
role, realm = sys.argv[1], sys.argv[2]
net = uninet.join("slicer-test-" + role, role=role, realm=realm)
got = []
net.subscribe("t.>", lambda message: got.append(message.data))
deadline = time.time() + 25
while time.time() < deadline and not net.peers():
    time.sleep(0.1)
for _ in range(60):
    net.publish("t.ping", {"from": role})
    time.sleep(0.1)
    if got:
        break
print("   ", role, "saw", [p.name for p in net.peers()], "and received", got[:1])
net.close()
sys.exit(0 if got else 1)
PY
# Under a timeout, because the failure this catches is not always a wrong
# answer: a session that deadlocks gives no answer at all, and a test that hangs
# for ever is worse than one that fails.
timeout 90 "$PYSLICER" -u /tmp/uninet-slicer-peer.py a "$REALM" & A=$!
timeout 90 "$PYSLICER" -u /tmp/uninet-slicer-peer.py b "$REALM" & B=$!
wait $A; RA=$?
wait $B; RB=$?
if [ $RA -eq 0 ] && [ $RB -eq 0 ]; then
    pass "messages arrived in both directions"
elif [ $RA -eq 124 ] || [ $RB -eq 124 ]; then
    fail "a peer never finished: the session is stuck, not merely silent"
else
    fail "peer exchange (a=$RA b=$RB)"
fi

# ── 4. the second install is the cheap one ───────────────────────────────────
step "Reinstalling from the wheel the first build left in the cache"
"$PYTHON" "$SETUP" uninstall --slicer "$SLICER" >/dev/null 2>&1
START=$(date +%s)
if "$PYTHON" "$SETUP" install --slicer "$SLICER" >/dev/null 2>&1; then
    ELAPSED=$(($(date +%s) - START))
    if [ "$ELAPSED" -le 30 ]; then
        pass "reinstall took ${ELAPSED}s: the cached wheel was used, nothing was compiled"
    else
        fail "reinstall took ${ELAPSED}s, so the wheel cache was not used"
    fi
else
    fail "reinstall"
fi

step "Installing again with UniNet already there"
if "$PYTHON" "$SETUP" install --slicer "$SLICER" 2>&1 | grep -q "already installed"; then
    pass "an install that has nothing to do says so and does nothing"
else
    fail "a redundant install did not report itself as one"
fi

# ── 5. the startup hook ──────────────────────────────────────────────────────
step "The startup hook"
RCFILE="$("$PYTHON" - "$SETUP" "$SLICER" <<'PY'
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(sys.argv[1]).parent))
import UniNetSlicer
print(UniNetSlicer.slicerrc_path(pathlib.Path(sys.argv[2])))
PY
)"
# What the file looks like with no hook in it. Taken with the hook stripped, so
# that running this twice compares against the same thing both times - and
# remembered, so a hook that was already there is put back at the end.
HAD_HOOK=no
[ -f "$RCFILE" ] && grep -q "UniNet (managed by" "$RCFILE" && HAD_HOOK=yes
"$PYTHON" "$SETUP" unhook --slicer "$SLICER" >/dev/null 2>&1
BEFORE=""
[ -f "$RCFILE" ] && BEFORE="$(cat "$RCFILE")"

"$PYTHON" "$SETUP" hook --slicer "$SLICER" >/dev/null 2>&1
"$PYTHON" "$SETUP" hook --slicer "$SLICER" >/dev/null 2>&1     # twice on purpose
if [ "$(grep -c "UniNet (managed by" "$RCFILE")" = "1" ]; then
    pass "installing the hook twice leaves one copy ($RCFILE)"
else
    fail "the hook was duplicated in $RCFILE"
fi
if "$PYTHON" -c "compile(open('$RCFILE').read(), 'slicerrc', 'exec')"; then
    pass "the rc file is valid Python, so Slicer will not stop on it"
else
    fail "the rc file does not compile"
fi

# Slicer itself, started with the hook in place. --no-splash and a script that
# exits immediately: what is being tested is that startup survives the hook.
if command -v xvfb-run >/dev/null && [ -x "$SLICER/Slicer" ]; then
    cat > /tmp/uninet-slicer-start.py <<'PY'
import sys, slicer
print("   startup reached the script with uninet",
      "importable" if __import__("importlib.util", fromlist=["util"]).find_spec("uninet") else "MISSING")
slicer.util.exit(0)
PY
    # To a file, not through a pipe: Slicer's launcher exits non-zero on a clean
    # shutdown often enough that its exit code says nothing, and under pipefail
    # that made a passing check report a failure.
    timeout 300 xvfb-run -a "$SLICER/Slicer" --no-splash --python-script \
        /tmp/uninet-slicer-start.py > /tmp/uninet-slicer-start.log 2>&1
    if grep -q "startup reached the script with uninet importable" /tmp/uninet-slicer-start.log; then
        pass "Slicer starts with the hook installed, and UniNet is importable"
    else
        fail "Slicer did not start cleanly with the hook installed"
        tail -15 /tmp/uninet-slicer-start.log
    fi
else
    printf '  (skipped the real Slicer startup: needs xvfb-run)\n'
fi

"$PYTHON" "$SETUP" unhook --slicer "$SLICER" >/dev/null 2>&1
if [ -z "$BEFORE" ]; then
    [ -s "$RCFILE" ] && fail "unhook left content behind in $RCFILE" || pass "unhook leaves the rc file as it was"
else
    [ "$(cat "$RCFILE")" = "$BEFORE" ] && pass "unhook leaves the rc file as it was" \
        || fail "unhook changed something else in $RCFILE"
fi

# Put back the hook if the developer running this had one. Testing something
# should not quietly turn off the thing being tested.
if [ "$HAD_HOOK" = yes ]; then
    "$PYTHON" "$SETUP" hook --slicer "$SLICER" >/dev/null 2>&1
    printf '  (restored the startup hook you already had)\n'
fi

# ── 6. the wheel, the thing you hand to a colleague ──────────────────────────
step "The redistributable wheel"
if WHEEL="$("$PYTHON" "$SETUP" wheel --slicer "$SLICER" 2>&1 | tail -1 | tr -d ' ')" && [ -f "$WHEEL" ]; then
    pass "wheel built: $(basename "$WHEEL")"
    case "$(basename "$WHEEL")" in
        *cp"$("$PYSLICER" -c 'import sys; print("%d%d" % sys.version_info[:2])')"*)
            pass "it carries this Slicer's Python tag" ;;
        *) fail "the wheel is tagged for another Python" ;;
    esac
else
    fail "wheel"
fi

printf '\n%s passed, %s failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
