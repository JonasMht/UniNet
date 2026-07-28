#!/usr/bin/env bash
# UniNet: one-command bootstrap for Linux & macOS.
#
# Installs build prerequisites (if a known package manager is present), then
# configures, builds, and tests UniNet. Safe to re-run.
#
#   ./scripts/bootstrap.sh           # C++ lib + tests + benchmark + C ABI
#   ./scripts/bootstrap.sh --python  # also build the Python extension (needs pip)
#
# Overrides (env):
#   UNINET_SKIP_DEPS=1     don't touch the system package manager
#   UNINET_NO_PYTHON=1     never build the Python extension
#   UNINET_BUILD_PYTHON=1  same as passing --python
set -euo pipefail

usage() {
    cat <<'USAGE'
bootstrap.sh: install prerequisites, build UniNet, run the tests.

  ./scripts/bootstrap.sh              C++ library, C ABI, tests
  ./scripts/bootstrap.sh --python     also build the Python extension
  ./scripts/bootstrap.sh --help       this message

Zyre is not packaged by Ubuntu, Debian or Fedora, so CMake fetches and builds it
on the first configure. Nothing else is needed from you.
USAGE
}
BUILD_PYTHON="${UNINET_BUILD_PYTHON:-0}"
for arg in "$@"; do
    case "$arg" in
        -h|--help) usage; exit 0 ;;
        --python)  BUILD_PYTHON=1 ;;
        # An unknown flag used to be ignored in silence, so a typo like
        # --with-python built without the extension and said nothing.
        *) echo "unknown option: $arg" >&2; echo; usage; exit 2 ;;
    esac
done


ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"


say() { printf '\033[1;34m▶ %s\033[0m\n' "$*"; }
ok()  { printf '\033[1;32m✓ %s\033[0m\n' "$*"; }

# ── 1. prerequisites ─────────────────────────────────────────────────────────
if [[ "${UNINET_SKIP_DEPS:-0}" != "1" ]]; then
  say "Checking build prerequisites"
  if command -v apt-get >/dev/null; then
    sudo apt-get update -y >/dev/null
    sudo apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config git zlib1g-dev liblz4-dev libzmq3-dev libczmq-dev >/dev/null
    ok "apt: build tools, zlib, lz4, libzmq, czmq installed (zyre is built from source)"
  elif command -v dnf >/dev/null; then
    sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config git zlib-devel lz4-devel zeromq-devel czmq-devel >/dev/null
    ok "dnf: build tools, zlib, lz4, zeromq, czmq installed (zyre is built from source)"
  elif command -v pacman >/dev/null; then
    sudo pacman -S --noconfirm --needed base-devel cmake pkgconf git zlib lz4 zeromq czmq zyre >/dev/null
    ok "pacman: build tools, zlib, lz4, zeromq, czmq, zyre installed"
  elif command -v brew >/dev/null; then
    brew install cmake pkg-config git zlib lz4 zeromq czmq zyre >/dev/null || true
    ok "brew: cmake, zlib, lz4, zeromq, czmq, zyre installed"
  else
    echo "No supported package manager found. Install manually: a C++17 compiler, cmake, pkg-config, zlib, lz4."
  fi
fi
command -v cmake >/dev/null || { echo "cmake not found: install it and re-run."; exit 1; }

# ── 2. Python extension? ─────────────────────────────────────────────────────
CMAKE_EXTRA=()
if [[ "$BUILD_PYTHON" == "1" && "${UNINET_NO_PYTHON:-0}" != "1" ]]; then
  if command -v pip >/dev/null; then
    pip install -q "pybind11>=2.12" >/dev/null 2>&1 || true
    if python3 -c "import pybind11" 2>/dev/null; then
      PYDIR="$(python3 -m pybind11 --cmakedir)"
      CMAKE_EXTRA+=("-DUNINET_BUILD_PYTHON=ON" "-Dpybind11_DIR=$PYDIR")
      say "Python extension will be built"
    else
      echo "pybind11 not importable: skipping Python extension."
    fi
  fi
fi

# ── 3. configure + build ────────────────────────────────────────────────────
say "Configuring (Release, C ABI on)"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUNINET_BUILD_CABI=ON "${CMAKE_EXTRA[@]:-}"

say "Building"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"

# ── 4. test ──────────────────────────────────────────────────────────────────
say "Running tests"
ctest --test-dir build -L uninet --no-tests=error --output-on-failure

ok "Done. C++ lib + C ABI are in build/. Try: ./build/uninet-benchmark 300"
if [[ "$BUILD_PYTHON" == "1" ]]; then
  ok "Python: PYTHONPATH=python python3 -c 'import uninet; print(uninet.__version__)'"
fi
# An `&&` list as the last command returns 1 when the test is false, so a
# perfectly successful default run (BUILD_PYTHON=0) exited non-zero and any
# wrapper or CI step read it as a failure.
exit 0
