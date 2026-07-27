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

// Ceiling on what one frame may decompress to. A compressed frame declares (LZ4)
// or implies (zlib) its decompressed size, and both numbers come from whoever
// sent the frame: a 34-byte LZ4 header claiming 2^64-1 bytes of content used to
// reach std::vector::resize and abort the process on length_error. Any frame that
// wants more than this is refused rather than sized up. 256 MiB is ~500x the
// largest payload UniNet actually carries (a 450 KiB mesh).
constexpr size_t MAX_DECOMPRESSED_BYTES = size_t(256) * 1024 * 1024;

// Compress / decompress a raw byte buffer. None is identity. Zlib is always
// available; Lz4 when compiled with liblz4 (UNINET_HAS_LZ4). A decompress of
// bytes that weren't compressed with `method` returns an empty buffer.
Bytes compress(const Bytes& raw, Compression method);
Bytes decompress(const Bytes& comp, Compression method);

// Same, into a caller-owned buffer (cleared, capacity retained). decompress_into()
// returns false (and clears `out`) on malformed, truncated, or over-large input —
// it never throws: it runs on the receive path, where every byte is hostile until
// proven otherwise, and its callers treat it as noexcept.
void compress_into(const uint8_t* raw, size_t n, Compression method, Bytes& out);
bool decompress_into(const uint8_t* comp, size_t n, Compression method, Bytes& out);

// Full wire frame of an envelope (header + compressed core). A uuid longer than
// the header's 16-bit length field cannot be framed: the result is an EMPTY wire
// (callers must treat empty as "not sent"). Truncating the length instead turned
// a targeted frame into a malformed broadcast.
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
