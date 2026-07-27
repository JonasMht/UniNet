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
#   UNINET_SKIP_DEPS=1   don't touch the system package manager
#   UNINET_NO_PYTHON=1   never build the Python extension
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_PYTHON=0
[[ "${1:-}" == "--python" || "${UNINET_BUILD_PYTHON:-0}" == "1" ]] && BUILD_PYTHON=1

say() { printf '\033[1;34m▶ %s\033[0m\n' "$*"; }
ok()  { printf '\033[1;32m✓ %s\033[0m\n' "$*"; }

# ── 1. prerequisites ─────────────────────────────────────────────────────────
if [[ "${UNINET_SKIP_DEPS:-0}" != "1" ]]; then
  say "Checking build prerequisites"
  if command -v apt-get >/dev/null; then
    sudo apt-get update -y >/dev/null
    sudo apt-get install -y --no-install-recommends \
      build-essential cmake pkg-config git zlib1g-dev liblz4-dev >/dev/null
    ok "apt: build tools + zlib + lz4 installed"
  elif command -v dnf >/dev/null; then
    sudo dnf install -y gcc-c++ cmake pkgconf-pkg-config git zlib-devel lz4-devel >/dev/null
    ok "dnf: build tools + zlib + lz4 installed"
  elif command -v pacman >/dev/null; then
    sudo pacman -S --noconfirm --needed base-devel cmake pkgconf zlib lz4 zyre >/dev/null
    ok "pacman: build tools + zlib + lz4 + zyre installed"
  elif command -v brew >/dev/null; then
    brew install cmake pkg-config zlib lz4 zyre >/dev/null || true
    ok "brew: cmake + zlib + lz4 installed"
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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUNINET_BUILD_CABI=ON "${CMAKE_EXTRA[@]}"

say "Building"
cmake --build build -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)"

# ── 4. test ──────────────────────────────────────────────────────────────────
say "Running tests"
ctest --test-dir build --output-on-failure

ok "Done. C++ lib + C ABI are in build/. Try: ./build/benchmark 4096 300"
[[ "$BUILD_PYTHON" == "1" ]] && ok "Python: PYTHONPATH=python python3 -c 'import uninet; print(uninet.__version__)'"
