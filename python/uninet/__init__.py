"""UniNet — brokerless peer-to-peer messaging, with discovery built in.

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
CBOR codec — see ``docs/PROTOCOL.md``.
"""
from __future__ import annotations

import contextlib
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
    headers: Optional[Dict[str, str]] = None,
    compression: Optional[Compression] = None,
) -> Session:
    """Join the network under ``name`` and return a :class:`Session`.

    This is the whole setup. Nothing else is required — no address, no port, no
    broker, no configuration file.

    Args:
        name: what other devices show for this one, e.g. ``"OR Headset"``.
        role: free-form label — ``"server"``, ``"headset"``, ``"viewer"``.
        app: the owning application, for a device list.
        realm: devices only see devices in the same realm. Change it to keep a
            development machine out of a live session, or to run two independent
            setups on one network.
        interface: only needed on a machine with several networks, where
            discovery could otherwise pick the wrong one (``"eth0"`` or an IP).
        port: UDP discovery port. Leave it alone unless it collides.
        headers: extra key/value advertised to peers, readable via
            ``peer.header(key)``.
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
    if headers:
        cfg.headers = dict(headers)
    if compression is not None:
        cfg.compression = compression
    return _join(name, cfg)


# Session is a C++ type, so the context-manager protocol is attached here rather
# than in the binding — it keeps the pybind11 layer to data and these to Python.
def _session_enter(self: Session) -> Session:
    return self


def _session_exit(self: Session, *exc: Any) -> bool:
    self.close()
    return False


def _session_close(self: Session) -> None:
    """Leave the network. Peers see the departure immediately.

    Called automatically when the session is garbage-collected or when a
    ``with`` block ends; call it explicitly for a prompt, predictable goodbye.
    """
    # The C++ destructor does the work; dropping the last reference is how it is
    # reached. Nothing to do here beyond making the intent explicit and letting
    # `with` blocks read naturally.
    pass


Session.__enter__ = _session_enter          # type: ignore[attr-defined]
Session.__exit__ = _session_exit            # type: ignore[attr-defined]
Session.close = _session_close              # type: ignore[attr-defined]


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
    "PROTOCOL_VERSION",
    "HAS_LZ4",
]
