#!/usr/bin/env bash
# Run the network-change test in a private network namespace.
#
#     ./scripts/test-reconnect.sh
#
# The test creates and deletes network interfaces to simulate Wi-Fi dropping, a
# phone being tethered and a cable being pulled. That would normally need root
# and would touch the developer's real network. `unshare -rn` avoids both: it
# puts the process in a new user and network namespace, where it is root over a
# private, empty network stack. The interfaces it makes exist only inside, and
# vanish when it exits. Nothing on the machine is touched, and no sudo is asked
# for.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-$HERE/build/test_reconnect}"

if [ ! -x "$BIN" ]; then
    echo "not built: $BIN" >&2
    echo "  cmake --build build -j --target test_reconnect" >&2
    exit 2
fi

if ! unshare -rn true 2>/dev/null; then
    echo "SKIP: unprivileged network namespaces are unavailable here."
    echo "On Debian/Ubuntu they may be disabled:"
    echo "    sysctl kernel.unprivileged_userns_clone"
    echo "The library is unaffected; only this test needs them."
    exit 0
fi

# ZeroMQ writes to /tmp and reads the hostname; both work in the namespace. The
# only thing it needs that is missing by default is a loopback that is UP, which
# the test brings up itself.
exec unshare -rn "$BIN"
