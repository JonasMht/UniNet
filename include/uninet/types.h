// UniNet — base value types. The library is transport-agnostic messaging: a peer
// (Node) publishes/subscribes Envelopes over a pluggable Transport, with a compact
// CBOR codec and optional compression. Consumers build Cbor payloads from their
// own message types; UniNet owns the framing, the codec, and the bus.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uninet {

// Uncompressed payload bytes (a serialized CBOR envelope, possibly compressed).
using Bytes = std::vector<uint8_t>;

// Bumped only on an incompatible wire change. Readers refuse unknown majors; the
// minor byte carries the in-place evolution room. See docs/PROTOCOL.md.
constexpr uint16_t CURRENT_PROTOCOL_VERSION = 1;

// Wire compression of the framed envelope. Negotiated per-message via the frame's
// 1-byte header — the field the three ThermoNav peers currently lack (each ships
// LZ4 code force-disabled to NONE because no peer can tell how a frame was encoded).
enum class Compression : uint8_t {
    None = 0,   // identity — always available
    Zlib = 1,   // deflate — always available (zlib)
    Lz4  = 2,   // LZ4 frame — optional (liblz4; auto-detected at build)
};

inline const char* compression_name(Compression m) {
    switch (m) {
        case Compression::None: return "none";
        case Compression::Zlib: return "zlib";
        case Compression::Lz4:  return "lz4";
    }
    return "?";
}

}  // namespace uninet
