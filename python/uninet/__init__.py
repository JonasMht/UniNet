"""UniNet: brokerless peer-to-peer messaging, with discovery built in.

Two devices on the same network find each other and talk. No broker to install,
no server to run, no IP address for anyone to type::

    import uninet

    net = uninet.join("Slicer Viewer", role="viewer")
    net.subscribe("domain.>", lambda msg: print(msg.subject, msg.data))
    net.publish("domain.D1", {"code": "update", "points": [1.0, 2.0, 3.0]})

    for peer in net.peers():
        print(peer.name, peer.address, peer.role)

A dict goes in and a dict comes out. The same dict published from Python arrives
as the same object in C++ and the same JSON in C#, because all three share one
CBOR codec (see ``docs/PROTOCOL.md``).
"""
from __future__ import annotations

import atexit
import contextlib
import os as _os
import sys as _sys
import weakref
from typing import Any, Dict, Iterator, Optional

try:
    from ._uninet import (  # noqa: F401
        HAS_LZ4,
        PROTOCOL_VERSION,
        Blob,
        BlobConfig,
        BlobInfo,
        Cbor,
        Compression,
        Envelope,
        LoopbackTransport,
        Message,
        Node,
        Peer,
        Session,
        SessionConfig,
        Transport,
        ZyreConfig,
        ZyreTransport,
        _join,
        decode,
        diagnostics,
        disable_crash_log,
        enable_crash_log,
        encode,
        from_json,
        local_hostname,
        local_interfaces,
        profiler_enable,
        profiler_report,
        profiler_reset,
        set_compression_level,
        to_json,
        zyre_version,
    )
except ImportError as _exc:  # pragma: no cover - only on a broken install
    # Half of UniNet is C++. If only the Python half got installed, the plain
    # error is "No module named 'uninet._uninet'", which says nothing about
    # what to do. Say it.
    raise ImportError(
        "UniNet's compiled extension (uninet._uninet) is missing or failed to "
        "load.\n"
        "\n"
        "The Python half of the package is installed but the C++ half is not, "
        "so nothing can work.\n"
        "\n"
        "If you installed with pip, pybind11 was most likely unavailable when "
        "the wheel was built:\n"
        "    pip install pybind11\n"
        "    pip install --force-reinstall --no-cache-dir <path to uninet>\n"
        "\n"
        "If you are running from a clone, the extension has to be built first:\n"
        "    cmake -S . -B build && cmake --build build -j\n"
        "    PYTHONPATH=python python -c 'import uninet'\n"
        "\n"
        f"The underlying error was: {_exc}"
    ) from _exc

__version__ = "0.2.0"
HAS_LZ4 = bool(HAS_LZ4)

# Which source this installed module was built from, when the build stamped it
# (UniNetSlicer.py embeds the git commit into every wheel it produces). Empty
# for any other build. This is the difference between "0.2.0" and *this* 0.2.0:
# the version never changes when fixes land, and without the commit nothing can
# tell a stale install from a current one.
__build__ = ""
try:  # generated at build time; absent in a plain source checkout
    from . import _buildinfo as _bi

    __build__ = getattr(_bi, "BUILD_GIT", "")
except Exception:  # noqa: BLE001 - an unreadable stamp must not break import
    pass


def version_info() -> Dict[str, Any]:
    """Everything that identifies THIS build of UniNet, as a dict.

    Two devices that cannot talk are usually two devices running different
    builds, and the version number alone does not say so: ``__version__`` does
    not change when a fix lands, so the same "0.2.0" can be three months apart
    on two machines. ``build`` is the git commit the wheel was made from, which
    does change, and ``protocol`` is the wire format -- a mismatch there means
    messages are decoded and then dropped, silently, by design (see
    ``Node::on_raw_``).

    Meant to be answered over the wire as well as printed. A peer that asks
    "which version are you" should be handed this verbatim::

        net.subscribe("app.version.request",
                      lambda m: net.publish("app.version", uninet.version_info()))
    """
    return {
        "version": __version__,
        # Empty in a source checkout, the git commit in anything UniNetSlicer
        # built. "0.2.0 (unstamped)" is itself worth seeing: it means this is
        # not an installed wheel.
        "build": __build__,
        "protocol": int(PROTOCOL_VERSION),
        "zyre": zyre_version(),
        "compression": "lz4" if HAS_LZ4 else "zlib",
        "host": local_hostname(),
        "python": _sys.version.split()[0],
        # Which copy of the package is actually imported. On a machine where a
        # stale install shadows a fresh one, this is the line that says so.
        "path": _os.path.dirname(_os.path.abspath(__file__)),
    }


def banner() -> str:
    """The version banner, as text. See :func:`join` for when it is printed."""
    info = version_info()
    build = info["build"] or "unstamped source checkout"
    return (
        f"UniNet {info['version']} (build {build})\n"
        f"  protocol v{info['protocol']} | {info['zyre']} | compression {info['compression']}\n"
        f"  python {info['python']} on {info['host'] or 'this machine'}\n"
        f"  from {info['path']}"
    )


# Printed once per process, not once per join: an application that joins twice
# should not print it twice, and one that never joins should not print it at
# all. Reset only by restarting the host.
_banner_printed = False


def print_banner(force: bool = False) -> None:
    """Print the version banner, once per process unless ``force``.

    WHY THIS EXISTS AT ALL. UniNet is used inside hosts with no terminal and no
    obvious place to look -- a 3D Slicer module, a Unity player. When something
    is wrong between two of them, the first question is always "which build is
    each side running", and until this printed, nothing anywhere answered it:
    the module loaded, the session joined, and the console said nothing. A
    silent successful start is indistinguishable from a silent stale one.
    """
    global _banner_printed
    if _banner_printed and not force:
        return
    _banner_printed = True
    print(banner(), flush=True)


def join(
    name: str,
    *,
    role: str = "",
    app: str = "",
    realm: str = "uninet",
    interface: str = "",
    port: int = 5670,
    gossip_bind: str = "",
    gossip_connect: str = "",
    endpoint: str = "",
    advertised_endpoint: str = "",
    headers: Optional[Dict[str, str]] = None,
    compression: Optional[Compression] = None,
    auto_reconnect: bool = True,
    reconnect_poll_ms: int = 2000,
    subscribe: Optional[Dict[str, Any]] = None,
    max_delivery_bytes: Optional[int] = None,
    max_delivery_messages: Optional[int] = None,
    deliver_on_network_thread: bool = False,
    banner: Optional[bool] = None,
) -> Session:
    """Join the network under ``name`` and return a :class:`Session`.

    This is the whole setup. Nothing else is required, no address, no port, no
    broker, no configuration file.

    Args:
        name: what other devices show for this one, e.g. ``"Headset"``.
        role: free-form label: ``"server"``, ``"headset"``, ``"viewer"``.
        app: the owning application, for a device list.
        realm: devices only see devices in the same realm. Change it to keep a
            development machine out of a live session, or to run two independent
            setups on one network.
        interface: only needed on a machine with several networks, where
            discovery could otherwise pick the wrong one (``"eth0"`` or an IP).
        port: UDP discovery port. Leave it alone unless it collides.
        gossip_bind: bind a rendezvous endpoint (``"tcp://*:5670"``) instead of
            using the UDP beacon. For links with no multicast: a USB-tethered
            device behind a port forward, a VPN, a routed network.
        gossip_connect: connect to another node's rendezvous endpoint.
        endpoint: this node's own data endpoint, in gossip mode.
        advertised_endpoint: what to tell peers this node's endpoint is, when
            that differs from what it binds (a forwarded port).
        headers: extra key/value advertised to peers, readable via
            ``peer.header(key)``. Pass the whole dict here; assigning into
            ``config.headers`` item by item does nothing, because the binding
            returns a fresh dict on every access.
        compression: wire compression. The default is the fastest tier the build
            has.
        auto_reconnect: rejoin by itself when the network changes underneath a
            running session (a cable, a new Wi-Fi, a VPN coming up). On by
            default; pass False to handle it yourself.
        reconnect_poll_ms: how often to look at the interfaces, in milliseconds.
        subscribe: register handlers at join time, atomically with the join:
            ``join("X", subscribe={"domain.>": handler})``. Subscribing at
            application start -- never from a UI callback, never after the
            first relevant messages -- is the pattern this library is built
            around, and the one that cannot lose messages: anything that
            arrives before a subscription is buffered by the session and
            delivered to the first matching one (see SessionConfig
            ``max_buffer_bytes`` / ``buffer_unmatched`` and ``Session.stats()``).
        max_delivery_bytes: how much undelivered traffic the delivery queue may
            hold before the oldest message is discarded (default 256 MiB; 0 is
            unlimited). Handlers run on their own thread, so a slow handler
            costs queue occupancy rather than lost messages -- but only up to
            this cap. See ``Session.delivery_stats()``.
        max_delivery_messages: the same cap counted in messages (default
            unlimited).
        deliver_on_network_thread: run handlers on the thread that reads the
            network. Leave this alone. A Python handler must take the GIL
            before it can run, so with this on, a busy main thread stalls the
            network reader and messages are dropped inside ZeroMQ before UniNet
            can see them -- which is exactly the "we lose packets while the UI
            is busy" failure the delivery thread exists to remove.
        banner: print the version banner on the first join in this process.
            Default (None) prints it unless ``UNINET_BANNER`` is set to ``0``.
            Pass False to stay quiet, True to print it every time.

            It is on by default because UniNet runs inside hosts with no
            terminal to check -- a Slicer module, a Unity player -- where
            "which build is this side running" was previously unanswerable and
            is the first question every cross-device problem asks.

    The returned session is also a context manager::

        with uninet.join("Tool") as net:
            net.publish("t.x", {"hello": True})
    """
    # Printed BEFORE the join, not after. If the network setup below throws or
    # hangs, the banner is the only thing that will have said which build was
    # even trying -- and a join that hangs is precisely when that matters.
    #
    # None means "the default": once per process, unless the host has asked for
    # quiet. An explicit True forces it, so a second join in a session that
    # already printed can still be made to say so.
    #
    # NOTE: this parameter shadows the module-level banner() function for the
    # body of join(). Nothing here calls it, and print_banner() resolves it in
    # its own scope; anything added below that needs the text must reach for
    # print_banner() rather than banner().
    if banner is None:
        if _os.environ.get("UNINET_BANNER", "1") != "0":
            print_banner()
    elif banner:
        print_banner(force=True)

    cfg = SessionConfig()
    cfg.role = role
    cfg.app = app
    cfg.realm = realm
    cfg.iface = interface
    cfg.port = port
    cfg.gossip_bind = gossip_bind
    cfg.gossip_connect = gossip_connect
    cfg.endpoint = endpoint
    cfg.advertised_endpoint = advertised_endpoint
    if headers:
        cfg.headers = dict(headers)
    cfg.auto_reconnect = auto_reconnect
    cfg.reconnect_poll_ms = reconnect_poll_ms
    cfg.deliver_on_network_thread = deliver_on_network_thread
    # Only assign when the caller asked: the C++ defaults are the documented
    # ones, and writing None through would be a type error rather than a
    # default.
    if max_delivery_bytes is not None:
        cfg.max_delivery_bytes = int(max_delivery_bytes)
    if max_delivery_messages is not None:
        cfg.max_delivery_messages = int(max_delivery_messages)
    if compression is not None:
        cfg.compression = compression   # bound now; this used to raise
    session = _join(name, cfg)
    if subscribe:
        for pattern, handler in subscribe.items():
            session.subscribe(pattern, handler)
    _live.add(session)
    return session


# Session is a C++ type, so the context-manager protocol is attached here rather
# than in the binding. It keeps the pybind11 layer to data and these to Python.
def _session_enter(self: Session) -> Session:
    return self


def _session_exit(self: Session, *exc: Any) -> bool:
    self.close()
    return False


Session.__enter__ = _session_enter          # type: ignore[attr-defined]
Session.__exit__ = _session_exit            # type: ignore[attr-defined]


# Every live session, so they can be closed before the process exits.
#
# WHY THIS EXISTS. ZeroMQ installs a C `atexit` handler that tears down its
# global context. A session still holding sockets when that runs makes czmq
# abort inside zsock_set_sndtimeo. Relying on the garbage collector is not
# enough: a session referenced by a module-level name, or captured by a
# subscriber callback, routinely outlives interpreter finalization.
#
# Python's own `atexit` runs during finalization, before the C handler, so
# closing here is correctly ordered. Weak references mean this never keeps a
# session alive that the caller has dropped.
_live: "weakref.WeakSet" = weakref.WeakSet()


def _close_all_sessions() -> None:
    for session in list(_live):
        try:
            session.close()
        except Exception:      # nothing useful to do while the process is exiting
            pass


atexit.register(_close_all_sessions)


@contextlib.contextmanager
def profiling() -> Iterator[None]:
    """Profile a block of work and print the per-operation breakdown.

    ::

        with uninet.profiling():
            for _ in range(1000):
                net.publish("t.x", payload)
    """
    profiler_reset()
    profiler_enable(True)
    try:
        yield
    finally:
        profiler_enable(False)
        print(profiler_report())


__all__ = [
    "join",
    "profiling",
    "banner",
    "print_banner",
    "version_info",
    "Session",
    "SessionConfig",
    "Peer",
    "Message",
    "Blob",
    "BlobInfo",
    "BlobConfig",
    "Cbor",
    "Compression",
    "Envelope",
    "Node",
    "Transport",
    "LoopbackTransport",
    "ZyreTransport",
    "ZyreConfig",
    "encode",
    "decode",
    "to_json",
    "from_json",
    "set_compression_level",
    "profiler_enable",
    "profiler_report",
    "profiler_reset",
    "zyre_version",
    "local_hostname",
    "local_interfaces",
    "diagnostics",
    "enable_crash_log",
    "disable_crash_log",
    "PROTOCOL_VERSION",
    "HAS_LZ4",
]
