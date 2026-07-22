# UniNet — canonical wire protocol

UniNet is a versioned pub/sub wire protocol for distributed medical-navigation
peers. One standard, one codec, one framing — owned by this project. This document
is authoritative; the C++/Python/C# implementations in this repo conform to it.

It exists because the three ThermoNav peers (MR/Unity-C#, Server/C++, Slicer/Python)
each re-implemented the *same* NATS+CBOR bus three times, with hand-mirrored schemas
and LZ4 code that nobody could enable. UniNet is that bus, written once.

## Goals

- **One contract** shared by every peer (server, MR headset, Slicer, future mesh node).
- **Transport-agnostic:** the framing/codec are independent of how bytes travel
  (NATS today; loopback for tests; mesh/BLE staged) via a `Transport` interface.
- **Negotiated compression:** a 1-byte header tells a receiver how the frame was
  encoded, so LZ4 can finally be turned on without breaking peers.
- **Forward-compatible:** a protocol version gates every frame.

## Wire frame

```
frame = [ 1 byte: compression ][ payload ]
payload = compression==None ? CBOR(envelope) : compress(CBOR(envelope))
```

`compression` is an unsigned byte: `0`=None, `1`=Zlib (deflate), `2`=LZ4 frame.
This single header byte is the field the ThermoNav peers lacked — each shipped LZ4
code force-disabled to None because no peer could tell how a frame was encoded.

## Envelope (CBOR map)

```
{
  "pv":  uint    protocol version (uint16; CURRENT_PROTOCOL_VERSION = 1)
  "cp":  uint    compression byte used for THIS frame (0/1/2; mirrors the header)
  "src": text    sender UUID
  "dst": text    "" = broadcast; else a targeted peer UUID (unicast)
  "sub": text    subject, e.g. "domain.D1" (NATS-style; ">" wildcard for subs)
  "data": <any>  the message payload (see "Message payloads")
}
```

A receiver accepts a frame when `src != self` (echo suppression) **and**
(`dst == ""` or `dst == self`). Addressing is by UUID in the envelope, not by
subject — the subject is the topic, the UUID is the route.

## Subjects

NATS-style, dot-separated:

| subject       | meaning                                              |
|---------------|------------------------------------------------------|
| `domain.D1`   | the shared OR bus (the one peer all three use today) |
| `<app>.<id>`  | a scoped stream, e.g. `telemetry.needle`             |
| `>`           | wildcard: matches one-or-more trailing tokens        |

Multiplexing by message *type* happens inside `data` (see payloads), not by
spawning many subjects — matching the existing `domain.D1` deployment.

## Message payloads

`data` is an arbitrary CBOR value. The ThermoNav application schema (the string
tags currently duplicated across the three peers) is migrated into
`include/uninet/schema.h` as the single source of truth:

```
data = { "code": <code>, "<code>_type": <type>, ...fields }
```

| tag family      | values (subset)                                                        |
|-----------------|------------------------------------------------------------------------|
| `code`          | update · request · information · message                               |
| `update_type`   | object · transform · remove · reset · events · material · state ·      |
|                 | stats · insertion_map · vertex_distances · safety_map · safety_texture ·|
|                 | result · metrics · info                                                |
| `request_type`  | applicator · sync · reset · new_case · volume · solve · metrics ·      |
|                 | start/stop/next/previous_procedure · start_experiment                  |
| `object_type`   | surface · applicator · volume                                          |

Mesh geometry rides as `{ "polydata": { "points": float32[], "polys": uint[] },
"transform": float64[16] }` — the shape `ThermoNavMR/SurfaceObject.cs` and
`ThermoNavServer/surface_object.cpp` hand-mirror today.

## Versions

| v  | change                                                                  |
|----|-------------------------------------------------------------------------|
| 1  | CURRENT. CBOR envelope + 1-byte compression header; UUID echo/dst filter.|

Readers refuse unknown `pv` majors. The compression tier (zlib/lz4) is a build- and
runtime-choice, not a protocol change — a None-only peer interoperates with an
LZ4 peer by simply sending None frames.

## Backends

| Transport           | status   | notes                                                 |
|---------------------|----------|-------------------------------------------------------|
| LoopbackTransport   | v0.1     | in-process; deterministic; tests + benchmarks         |
| NatsTransport       | optional | the production brokered bus (`nats://…:4222`); gated  |
| MeshTransport       | staged   | peer discovery + relay behind the same interface      |
| BleTransport        | staged   | Bluetooth-LE device bridge                            |
