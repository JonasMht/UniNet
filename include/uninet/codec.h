// UniNet — envelope framing + compression. The Envelope is the one wire contract
// every peer shares: who sent it, who it's for, what subject, and an arbitrary
// Cbor payload. `frame()` produces the on-wire bytes; `unframe()` parses them.
//
// Wire format (see docs/PROTOCOL.md) — routing in a CLEAR header, content
// compressed, so a receiver can filter echoes/targeting WITHOUT decompressing:
//
//   frame = [ comp:1 ][ flags:1 ][ srclen:2 BE ][ src ][ dstlen:2 BE ][ dst ][ payload ]
//   payload = comp==None ? encode(core) : compress(encode(core))
//   core    = { "pv": uint, "sub": text, "data": <any> }   // src/dst live in the header
//
// This split is what lets Node::on_raw_ drop a peer's own echo (and messages not
// addressed to it) at the cost of reading ~tens of header bytes — no decompress,
// no CBOR decode. With compression on (the recommended config) that is the
// difference between decoding every frame twice and decoding it once.
#pragma once

#include "uninet/cbor.h"
#include "uninet/types.h"

#include <optional>
#include <string>

namespace uninet {

struct Envelope {
    uint16_t protocol_version = CURRENT_PROTOCOL_VERSION;
    Compression compression = Compression::None;
    std::string src_uuid;          // sender (wire: header)
    std::string dst_uuid;          // "" = broadcast; else targeted unicast (wire: header)
    std::string subject;           // e.g. "domain.D1" (wire: core)
    Cbor data;                     // arbitrary message payload (wire: core)
};

// Core (subject + data + version) <-> Cbor. Routing (src/dst/compression) is NOT
// in the Cbor — it rides in the binary header so it stays readable when compressed.
Cbor to_cbor(const Envelope& e);
std::optional<Envelope> from_cbor(const Cbor& c);   // reads core; src/dst/compression left default

// Routing lifted from the wire header — cheap, no decompress/decode.
struct Routing {
    Compression compression = Compression::None;
    std::string src;
    std::string dst;
};
std::optional<Routing> peek_routing(const Bytes& wire);

// Deflate level for the zlib codec (1..9). Lower = faster + worse ratio. Default 6.
void set_compression_level(int level);

// Compress / decompress a raw byte buffer. None is identity. Zlib is always
// available; Lz4 when compiled with liblz4 (UNINET_HAS_LZ4). A decompress of
// bytes that weren't compressed with `method` returns an empty buffer.
Bytes compress(const Bytes& raw, Compression method);
Bytes decompress(const Bytes& comp, Compression method);

// Same, into a caller-owned buffer (cleared, capacity retained).
void compress_into(const uint8_t* raw, size_t n, Compression method, Bytes& out);
bool decompress_into(const uint8_t* comp, size_t n, Compression method, Bytes& out);

// Full wire frame of an envelope (header + compressed core).
Bytes frame(const Envelope& e);
std::optional<Envelope> unframe(const Bytes& wire);

// ── allocation-free hot path ──
// frame()/unframe() each allocate and free their intermediate buffers. Above
// glibc's (dynamic, initially 128 KiB) mmap threshold every such buffer is an
// mmap/munmap syscall pair plus page faults on first touch — for the mesh-sized
// payloads UniNet actually carries (100–450 KiB) that allocator traffic costs
// more than the encode and the compression combined. These overloads let a
// steady-state publisher/receiver reuse one set of buffers and allocate nothing.
//
// Reuse one Scratch per thread (it is not synchronized).
struct Scratch {
    Bytes core;      // encoded CBOR core
    Bytes payload;   // compressed core (unused when Compression::None)
};

// Build the wire frame into `wire` (cleared, capacity retained).
void frame_into(const Envelope& e, Bytes& wire, Scratch& scratch);

// Parse a wire frame held elsewhere. The uncompressed payload is decoded in
// place — no copy — and only the compressed case touches `scratch`.
std::optional<Envelope> unframe_into(const uint8_t* wire, size_t n, Scratch& scratch);

}  // namespace uninet
