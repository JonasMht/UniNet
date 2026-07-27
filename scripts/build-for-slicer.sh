#!/usr/bin/env bash
# Build UniNet's Python extension for 3D Slicer, and install it.
#
#     ./scripts/build-for-slicer.sh /path/to/Slicer-5.8.1-linux-amd64
#
# Slicer ships its own Python, so the extension has to be built against THAT
# interpreter: one built for the system Python will not load. Three things about
# a Slicer binary release make this awkward, and this script handles all of them.
#
#  1. Slicer's `python-real` cannot run on its own. It needs PYTHONHOME and
#     LD_LIBRARY_PATH pointing into the Slicer tree, or it fails with
#     "could not find platform independent libraries".
#
#  2. **The binary release ships no Python headers.** lib/Python/include/python3.9
#     contains only pyconfig.h. Everything else is missing, so the build fails on
#     "Python.h: No such file or directory". The fix is to fetch the matching
#     CPython source headers and combine them with Slicer's pyconfig.h. Slicer's
#     Python is stock CPython, so upstream headers match exactly.
#
#  3. pybind11 finds its own interpreter unless told otherwise, and passing
#     -DPython3_EXECUTABLE is not enough for the legacy FindPythonLibsNew path
#     that Slicer's pybind11 takes. PYTHON_EXECUTABLE plus an explicit -I is.
#
# Verified against Slicer 5.8.1 (Python 3.9.10) on Linux.
set -euo pipefail

SLICER="${1:-}"
if [ -z "$SLICER" ] || [ ! -x "$SLICER/bin/PythonSlicer" ]; then
    cat >&2 <<USAGE
usage: $0 /path/to/Slicer-X.Y.Z-linux-amd64

Give the directory that contains bin/PythonSlicer. On a default install that is
where you unpacked the release, e.g. ~/Documents/Slicer-5.8.1-linux-amd64.
USAGE
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYSLICER="$SLICER/bin/PythonSlicer"
PYREAL="$SLICER/bin/python-real"

PYVER="$("$PYSLICER" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
PYFULL="$("$PYSLICER" -c 'import sys; print("%d.%d.%d" % sys.version_info[:3])')"
SITE="$("$PYSLICER" -c 'import site; print(site.getsitepackages()[0])')"
echo "Slicer Python : $PYFULL"
echo "site-packages : $SITE"

export PYTHONHOME="$SLICER/lib/Python"
export LD_LIBRARY_PATH="$SLICER/lib/Python/lib:$SLICER/lib/Slicer-$(echo "$SLICER" | grep -oE '[0-9]+\.[0-9]+' | head -1):${LD_LIBRARY_PATH:-}"

# ── headers ───────────────────────────────────────────────────────────────
HDR="$HERE/build-slicer/pyheaders"
if [ ! -f "$HDR/Python.h" ]; then
    echo "Slicer ships no Python headers; fetching CPython $PYFULL to match..."
    mkdir -p "$HDR.tmp" && cd "$HDR.tmp"
    curl -fsSL "https://www.python.org/ftp/python/$PYFULL/Python-$PYFULL.tgz" -o py.tgz
    tar xzf "py.tgz" "Python-$PYFULL/Include"
    mkdir -p "$HDR"
    cp -r "Python-$PYFULL/Include/"* "$HDR/"
    # Slicer's own pyconfig.h, so the build matches how ITS Python was compiled.
    cp "$SLICER/lib/Python/include/python$PYVER/pyconfig.h" "$HDR/"
    cd "$HERE" && rm -rf "$HDR.tmp"
fi
echo "headers       : $HDR"

# ── build ─────────────────────────────────────────────────────────────────
cd "$HERE"
cmake -S . -B build-slicer -DCMAKE_BUILD_TYPE=Release -DUNINET_BUILD_PYTHON=ON \
      -DPYTHON_EXECUTABLE="$PYREAL" \
      -DPYTHON_INCLUDE_DIR="$HDR" \
      -DPYTHON_LIBRARY="$SLICER/lib/Python/lib/libpython$PYVER.so" \
      -DCMAKE_CXX_FLAGS="-I$HDR" \
      -Dpybind11_DIR="$("$PYSLICER" -m pybind11 --cmakedir)" >/dev/null
cmake --build build-slicer -j"$(nproc 2>/dev/null || echo 4)" --target _uninet

EXT="$(ls "$HERE"/python/uninet/_uninet.cpython-${PYVER/./}*.so 2>/dev/null | head -1)"
[ -n "$EXT" ] || { echo "the extension was not produced" >&2; exit 1; }

# ── install ───────────────────────────────────────────────────────────────
# Replaced wholesale, not merged: a leftover extension from an older Python or
# an older UniNet in this directory would shadow the new one, and the failure
# looks like "my changes did nothing".
rm -rf "$SITE/uninet"
mkdir -p "$SITE/uninet"
cp "$HERE/python/uninet/__init__.py" "$SITE/uninet/"
cp "$EXT" "$SITE/uninet/"

echo
echo "installed to  : $SITE/uninet"
"$PYSLICER" -c "
import uninet
print('verified      : uninet', uninet.__version__, '|', uninet.zyre_version())
"
echo
echo "Use it from a Slicer module with:  import uninet"
