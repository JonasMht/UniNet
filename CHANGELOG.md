# Changelog

What changed for someone using UniNet, newest first. Every entry is additive:
existing code compiles and behaves as before unless the entry says otherwise.
The commit history has the reasoning behind each change.

## Unreleased (branch `thermonav-v1.1`)

Additions asked for by the ThermoNav protocol v1.1. No wire-format change: a
peer running an earlier build interoperates with one running this.

### Added

- **One message to several peers.** `Session::publish_many(subject, data,
  dsts)` and `publish_many_json`, Python `Session.publish_many` /
  `publish_many_json`, C# `Session.PublishMany` / `PublishManyCbor`, C
  `uninet_session_publish_many_json` / `_cbor`. The payload is framed and
  compressed once and the same bytes are whispered to each listed peer; nobody
  else receives it. Returns how many peers it was handed to. Unknown uuids are
  skipped, repeats and empty strings ignored, and an empty list sends nothing
  (it never becomes a broadcast). Receivers see a message with no destination.
- **Peer timeouts.** `SessionConfig::evasive_ms` / `expired_ms` (defaults
  5000 / 30000, as before), Python `join(evasive_ms=, expired_ms=)` and
  `SessionConfig` fields, C# `Join(evasiveMs:, expiredMs:)`, C
  `uninet_config_set_timeouts`. Lower them to notice a device that dropped off
  the network within seconds (2000 / 6000: about 7 s). The C ABI and Python
  reject `expired_ms <= evasive_ms`.
- **Delivery-queue overflow policy, opt-in.** `SessionConfig::delivery_overflow`
  (`DeliveryOverflow::Oldest`, the default, or `OldestSameSubject`), Python
  `join(delivery_overflow=)` / `uninet.DeliveryOverflow`, C#
  `Join(deliveryOverflow:)` / `UniNet.DeliveryOverflow`, C
  `uninet_config_set_delivery_overflow`. With `OldestSameSubject`, a full
  queue discards the oldest message on the arriving message's subject before
  anything else. Python `join()` also gained `delivery_block_ms=`, which only
  C++ and C# could set before.
- **Every header on presence events, from C.** `uninet_session_on_peer_found_ex`
  / `_lost_ex` hand the callback a one-entry `uninet_peers_t` snapshot, so all
  of a peer's headers are readable at the moment it appears.

### Changed

- **C#: `PeerFound` / `PeerLost` carry every header** the peer advertised
  (`Peer.Header`, `Peer.Headers`), as `Peers()` always did. They used to carry
  role and app only, so a protocol version or capability list advertised at
  join could not be read from the event. The binding now needs a native
  library that exports `uninet_session_on_peer_found_ex`, i.e. one built from
  this version, as with any binding update.
- **C#, `marshalToCaller: false` only:**
  - adding a `PeerFound` handler replays the devices already present to it,
    as C++ and Python do. A device found in the milliseconds between `Join`
    and `PeerFound +=` used to be announced to nobody.
  - no handler runs on the thread that called `Subscribe`/`SubscribeCbor` or
    added `PeerFound`. Messages held from before the subscription used to be
    delivered during the `Subscribe` call, on the caller's (Unity: main)
    thread; they are now delivered on a short-lived UniNet thread while the
    caller waits.
  The default queued mode (`Update()`) is unchanged.

### Documented

- The cost of the delivery queue's default "wait up to 5 s when full": the
  network thread reads nothing while it waits, which suits `Blob` transfers and
  not live state. `delivery_block_ms = 0` with `OldestSameSubject` is the
  real-time setting (session.h, zyre_transport.h, cabi.h, README "Real-time
  traffic and the delivery queue").
- C#: decoding off Unity's main thread with `marshalToCaller: false` and a
  `ConcurrentQueue` drained in `Update()` (Session.cs header, README "Unity /
  Meta Quest").

### Tests

- `test_network`: `publish_many` sends byte-identical frames to the listed
  peers only and frames once (profiler count 1, against 2 for a loop);
  a SIGKILLed peer is reported lost in ~4 s with `expired_ms = 3000` (30 s with
  the defaults).
- `test_delivery`: with `delivery_block_ms = 0` and `OldestSameSubject` a flood
  never blocks the network thread and a one-off message on another subject
  survives; the same flood under the default policy evicts it.
- `test_cabi`: the new C functions, argument validation, and headers on
  `on_peer_found_ex`.
- Python: `publish_many`, `tn.*` headers on `peers()` and `on_peer_found`,
  configuration and validation, fast expiry of a killed child process.
- `scripts/test-interop.sh`: every participant advertises `tn.proto`, `tn.kind`,
  `tn.pid`, `tn.name` (non-ASCII) and `tn.caps` and checks the other two
  languages' arrive intact; the C# participant also checks `PeerFound`
  headers, that `SubscribeCbor` handlers (live and buffered) run off the main
  thread, and that the main-thread handoff sees every decoded message.
