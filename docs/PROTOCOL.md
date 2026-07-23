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
- **Routing in the clear, content compressed:** `src`/`dst`/compression ride in a
  binary header *before* the (possibly compressed) payload, so a receiver can drop
  its own echo and frames not addressed to it **without decompressing or decoding**.
- **Negotiated compression:** the header tells a receiver how the core was encoded,
  so LZ4 can finally be turned on without breaking peers.
- **Forward-compatible:** a protocol version gates every frame.

## Wire frame

```
frame = [ comp:1 ][ flags:1 ][ srclen:2 BE ][ src ][ dstlen:2 BE ][ dst ][ payload ]
payload = comp==None ? CBOR(core) : compress(CBOR(core))
core    = { "pv": uint, "sub": text, "data": <any> }     # routing is NOT here
```

`comp` is an unsigned byte: `0`=None, `1`=Zlib (deflate), `2`=LZ4 frame. `flags`
is reserved (0). Lengths are 2-byte big-endian. `src`/`dst` are the sender /
intended-receiver UUIDs (`dst` empty = broadcast).

This split is what the old single-byte-header design couldn't do: with compression
on, `src`/`dst` would be trapped inside the compressed blob, forcing every receiver
(including the sender hearing its own echo) to decompress + decode just to decide
whether to keep the frame. Keeping routing in the clear makes echo suppression and
unicast targeting nearly free — the profiler showed `unframe` running 4000× for
2000 publishes before this change; it now runs 2000×.

## Envelope core (CBOR map)

```
{
  "pv":  uint    protocol version (uint16; CURRENT_PROTOCOL_VERSION = 1)
  "sub": text    subject, e.g. "domain.D1" (NATS-style; ">" wildcard for subs)
  "data": <any>  the message payload (see "Message payloads")
}
```

`src`, `dst` and `compression` are reconstructed by `unframe()` from the binary
header — they are not in the CBOR core. (They were in v0.1's single-header form;
the split is the only in-place evolution since then and is wire-incompatible with
that early internal revision, which no deployed peer used.)

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

`data` is an arbitrary CBOR value. UniNet's transport and codec layers are
**schema-agnostic** — they carry any `Cbor` payload and impose no application
taxonomy. The ThermoNav application schema (the `code`/`*_type` string tags) is a
*consumer* concern, owned by a single IDL, `ThermoNavServer/prototypes/comm_standard/schema.toml`,
whose `gen_schema.py` emits the C++/Python/C# bindings for every peer. That IDL is
the one source of truth for the tags; UniNet does not duplicate it. The shape:

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
| `solver_type`   | fdm · fdm_multi_res · c_nca                                            |
| `information_type` | simulation                                                          |

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
