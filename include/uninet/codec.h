// UniNet — envelope framing + compression. The Envelope is the one wire contract
// every peer shares: who sent it, who it's for, what subject, and an arbitrary
// Cbor payload. `frame()` produces the on-wire bytes; `unframe()` parses them.
//
// Wire format (see docs/PROTOCOL.md):
//   frame = [ 1 byte: compression ][ payload ]
//   payload = compression==None ? encode(envelope) : compress(encode(envelope))
//
// The compression header byte is the single negotiation field that lets a receiver
// always decode — fixing the bug where ThermoNavMR/Server/Slicer each ship LZ4
// code but force it off (a peer can't tell how a frame was encoded).
#pragma once

#include "uninet/cbor.h"
#include "uninet/types.h"

#include <optional>
#include <string>

namespace uninet {

struct Envelope {
    uint16_t protocol_version = CURRENT_PROTOCOL_VERSION;
    Compression compression = Compression::None;
    std::string src_uuid;          // sender
    std::string dst_uuid;          // "" = broadcast; else targeted unicast
    std::string subject;           // e.g. "domain.D1"
    Cbor data;                     // arbitrary message payload
};

Cbor to_cbor(const Envelope& e);                       // envelope -> Cbor map
std::optional<Envelope> from_cbor(const Cbor& c);      // Cbor map -> envelope

// Deflate level for the zlib codec (1..9). Lower = faster + worse ratio. Default 6.
void set_compression_level(int level);

// Compress / decompress a raw byte buffer. None is identity. Zlib is always
// available; Lz4 when compiled with liblz4 (UNINET_HAS_LZ4). A decompress of
// bytes that weren't compressed with `method` returns an empty buffer.
Bytes compress(const Bytes& raw, Compression method);
Bytes decompress(const Bytes& comp, Compression method);

// Full wire frame of an envelope (uses e.compression).
Bytes frame(const Envelope& e);
std::optional<Envelope> unframe(const Bytes& wire);

}  // namespace uninet
