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
import weakref
from typing import Any, Dict, Iterator, Optional

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
    encode,
    from_json,
    local_hostname,
    profiler_enable,
    profiler_report,
    profiler_reset,
    set_compression_level,
    to_json,
    zyre_version,
)

__version__ = "0.2.0"
HAS_LZ4 = bool(HAS_LZ4)


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

    The returned session is also a context manager::

        with uninet.join("Tool") as net:
            net.publish("t.x", {"hello": True})
    """
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
    if compression is not None:
        cfg.compression = compression   # bound now; this used to raise
    session = _join(name, cfg)
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
    "PROTOCOL_VERSION",
    "HAS_LZ4",
]
