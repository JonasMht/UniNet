#!/usr/bin/env bash
# Kept so the command in older documentation still works. The real thing is
# scripts/UniNetSlicer.py, which does the same job on Linux, macOS and Windows,
# finds Slicer by itself, reuses a prebuilt wheel when there is one, and can
# check at every Slicer start that UniNet is still installed.
#
#     ./scripts/build-for-slicer.sh [/path/to/Slicer-5.8.1-linux-amd64]
#
# is now exactly:
#
#     python3 scripts/UniNetSlicer.py install [--slicer /path/to/Slicer...]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SLICER="${1:-}"

ARGS=(install)
[ -n "$SLICER" ] && ARGS+=(--slicer "$SLICER")

PYTHON="$(command -v python3 || command -v python || true)"
if [ -z "$PYTHON" ]; then
    # No system Python: Slicer's own runs the installer just as well.
    for candidate in "$SLICER" "$HOME"/Documents/Slicer-* "$HOME"/Slicer-* /opt/Slicer-*; do
        if [ -x "$candidate/bin/PythonSlicer" ]; then
            PYTHON="$candidate/bin/PythonSlicer"
            break
        fi
    done
fi
[ -n "$PYTHON" ] || {
    echo "No Python found. Use Slicer's own:" >&2
    echo "    <Slicer>/bin/PythonSlicer $HERE/UniNetSlicer.py install" >&2
    exit 1
}

echo "note: this now calls scripts/UniNetSlicer.py, which is the one to use."
exec "$PYTHON" "$HERE/UniNetSlicer.py" "${ARGS[@]}"
