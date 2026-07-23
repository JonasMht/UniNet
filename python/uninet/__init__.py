"""UniNet — Unified Networking transport.

Python front-end to the compiled C++ core (``_uninet``). Build Cbor payloads,
publish/subscribe over a Node on a LoopbackTransport (NATs backend staged), and
profile — through the same code path the C++ server and the C# client use.

See ``docs/PROTOCOL.md`` for the wire standard. The standard is owned by this
project; consumers (ThermoNavServer, ThermoNavMR, ThermoNavSlicer) depend on
UniNet rather than each re-implementing the bus.
"""
from __future__ import annotations

from ._uninet import (  # noqa: F401
    HAS_LZ4,
    HAS_NATS,
    Compression,
    Cbor,
    Envelope,
    LoopbackTransport,
    Node,
    PROTOCOL_VERSION,
    compress,
    decode,
    decompress,
    encode,
    set_compression_level,
)
from ._uninet import profiler  # opt-in bottleneck analytics submodule  # noqa: F401

__version__ = "0.1.0"
HAS_LZ4 = bool(HAS_LZ4)
HAS_NATS = bool(HAS_NATS)

# NatsTransport only exists in a NATS-enabled build.
try:  # pragma: no cover - depends on build flags
    from ._uninet import NatsTransport  # noqa: F401
except ImportError:
    NatsTransport = None

__all__ = [
    "Compression",
    "Cbor",
    "Envelope",
    "LoopbackTransport",
    "Node",
    "encode",
    "decode",
    "compress",
    "decompress",
    "set_compression_level",
    "profiler",
    "PROTOCOL_VERSION",
    "HAS_LZ4",
    "HAS_NATS",
]
