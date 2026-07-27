# UniNet: wire protocol

UniNet is a brokerless pub/sub protocol for peers on a local network. This
document is authoritative; the C++/Python/C# implementations conform to it.

There are two layers, and they are independent:

1. **Discovery and delivery**: ZeroMQ's **ZRE** protocol
   ([RFC 36](https://rfc.zeromq.org/spec/36/) / [RFC 43](https://rfc.zeromq.org/spec/43/)),
   implemented by [zeromq/zyre](https://github.com/zeromq/zyre). UniNet does not
   define this layer; it uses it.
2. **The envelope**: UniNet's own CBOR framing, carried inside a ZRE message.
   This is what the rest of the document specifies.

## Goals

- **Nothing is configured.** No addresses, ports, or broker endpoints in any
  application. A peer is created with a name.
- **One contract** shared by every peer (C++ server, Python module, C# headset).
- **Routing in the clear, content compressed**, so a receiver can filter a frame
  without decompressing or decoding it.
- **Negotiated compression**, so a peer can tell how a frame was encoded.
- **Forward-compatible:** a protocol version gates every frame.

## Layer 1: ZRE (discovery and delivery)

| | |
|---|---|
| Discovery | UDP beacon on port **5670**, hop limit 1 (link-local; never crosses a router) |
| Delivery | direct TCP between peers, ephemeral ports |
| Grouping | every UniNet node joins one ZRE **group**, named after the realm (default `uninet`) |
| Broadcast | `SHOUT` to the group |
| Unicast | `WHISPER` to one peer uuid |
| Presence | `ENTER` / `JOIN` / `LEAVE` / `EXIT` events |
| Identity | a 32-hex-character uuid assigned by ZRE, stable for the process |
| Metadata | ZRE **headers**, sent once with the beacon: UniNet uses `role`, `app`, plus anything the application sets |

Two consequences worth stating explicitly:

- **A node never receives its own broadcast.** ZRE does not echo, so UniNet needs
  no echo-suppression pass on the receive path.
- **`ENTER` fires for every node on the beacon port, regardless of group.** Realm
  isolation therefore keys on `JOIN`/`LEAVE` of the realm group, not on `ENTER` -
  otherwise a peer in another realm would appear in the device list.

### ZRE message shape

A UniNet message is a two-frame ZRE message:

```
frame 0: subject          (UTF-8 text, e.g. "domain.D1")
frame 1: UniNet envelope  (binary, see below)
```

Keeping the subject in its own frame lets a receiver match subscriptions without
decompressing or decoding the payload.

## Layer 2: the UniNet envelope

```
envelope = [ comp:1 ][ flags:1 ][ srclen:2 BE ][ src ][ dstlen:2 BE ][ dst ][ payload ]
payload  = comp==None ? CBOR(core) : compress(CBOR(core))
core     = { "pv": uint, "sub": text, "data": <any> }     # routing is NOT here
```

`comp` is an unsigned byte: `0`=None, `1`=Zlib (deflate), `2`=LZ4 frame. `flags`
is reserved (0). Lengths are 2-byte big-endian and are rejected above 65535.
`src`/`dst` are peer uuids; `dst` empty means broadcast.

Routing rides in a **clear** header before the compressed body. With compression
on, `src`/`dst` inside the compressed blob would force every receiver to
decompress and decode a frame just to decide whether to keep it.

### Envelope core (CBOR map)

```
{
  "pv":  uint    protocol version (uint16; CURRENT_PROTOCOL_VERSION = 1)
  "sub": text    subject, e.g. "domain.D1"
  "data": <any>  the message payload
}
```

`src`, `dst` and `compression` are reconstructed from the binary header; they are
not in the CBOR core.

A receiver accepts a frame when `dst == ""` or `dst == self`. (Echo suppression
is unnecessary; see above.

## Subjects

Dot-separated, with a trailing `>` wildcard for subscriptions:

| subject | meaning |
|---|---|
| `domain.D1` | exact match |
| `domain.>` | matches `domain.D1`, `domain.D2.feed`, ... (one or more trailing tokens) |
| `>` | matches everything |

`>` is the **only** wildcard. There is no `*`; a pattern containing one matches
nothing, rather than silently behaving differently from a subscriber's
expectation.

Multiplexing by message *type* happens inside `data`, not by spawning many
subjects.

## Message payloads

`data` is an arbitrary CBOR value. UniNet's transport and codec layers are
**schema-agnostic**. They carry any value and impose no application taxonomy.

The CBOR subset UniNet encodes and decodes:

| CBOR | notes |
|---|---|
| uint / negative int | negative arguments ≥ 2⁶³ are rejected rather than silently wrapped |
| byte string, text string | text is not UTF-8-validated; length-checked without overflow |
| array, map | insertion-ordered maps; element counts are bounds-checked against the remaining input |
| bool, null | |
| float16 / float32 / float64 | half-floats are **decoded** (some CBOR libraries emit them for exact values) but never emitted |
| homogeneous float arrays | a fast path: stored and encoded contiguously, no per-element node |

Reserved and unassigned major-7 values (`ai` 28–30) are rejected.

### JSON equivalence

`from_json` / `to_json` map JSON onto this subset so that a Python dict, a C#
JSON string and a C++ `Cbor` describing the same thing produce identical wire
bytes. JSON is the smaller type system, so:

- byte strings render as base64 text
- float arrays render as ordinary JSON arrays
- NaN and Infinity render as `null`
- integers beyond 2⁵³ survive CBOR exactly but lose precision in JSON consumers

## Compression

The tier is a per-message choice carried in the header, not a protocol change. A
None-only peer interoperates with an LZ4 peer by sending None frames.

| tier | when |
|---|---|
| LZ4 | live traffic (the default where liblz4 is present) |
| zlib | archival/batch: better ratio, far slower |
| None | always available |

Decompression is bounded: a frame declaring an implausible decompressed size is
rejected rather than attempted, and truncated LZ4 frames are refused rather than
accepted as complete.

## Versions

| v | change |
|---|---|
| 1 | CBOR envelope + compression header; uuid dst filter. Carried over ZRE from v0.2. |

Readers refuse unknown `pv` majors.

**v0.2 is not wire-compatible with v0.1's NATS deployment**: the transport
changed from a broker to peer-to-peer ZRE. The envelope itself is unchanged, so
message-handling code ports as-is. Migrate all peers together.

## Security

There is **no authentication or integrity check on the envelope**. `src` is
self-asserted, so `dst` targeting is spoofable by any peer that has joined the
realm. Realms are a scoping mechanism, not a security boundary.

Discovery beacons carry a hop limit of 1 and never leave the local link, which
bounds exposure to devices already on the same network segment. Treat that
network as the trust boundary.

Hardening that *is* in place, because every frame is unauthenticated input:

- length prefixes are range-checked without integer overflow
- decode recursion is capped (128 levels)
- container element counts are checked against the remaining input, so a small
  compressed frame cannot expand into an unbounded allocation
- decompressed size is capped against a policy maximum
- ZRE itself supports CURVE encryption; UniNet does not currently expose it

## Backends

| Transport | status | notes |
|---|---|---|
| `ZyreTransport` | current | brokerless peer-to-peer over ZRE; discovery included |
| `LoopbackTransport` | current | in-process, deterministic; tests and benchmarks |
| `NatsTransport` | **removed in v0.2** | required a broker and a configured address |
