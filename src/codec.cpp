// UniNet — envelope framing + compression implementation.
#include "uninet/codec.h"
#include "uninet/profiler.h"

#include <zlib.h>

#ifdef UNINET_HAS_LZ4
#include <lz4frame.h>
#endif

#include <cstring>

namespace uninet {

namespace {
int g_zlib_level = 6;  // zlib default compression level (1..9).
}

void set_compression_level(int level) {
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    g_zlib_level = level;
}

// ── envelope core <-> cbor (subject/data/version only; routing is in the header) ──
Cbor to_cbor(const Envelope& e) {
    return Cbor::map()
        .set("pv", Cbor::uint(e.protocol_version))
        .set("sub", Cbor::text(e.subject))
        .set("data", e.data);
}

std::optional<Envelope> from_cbor(const Cbor& c) {
    if (!c.is_map() || !c.has("sub") || !c.has("data"))
        return std::nullopt;
    Envelope e;
    e.protocol_version = c.has("pv") ? uint16_t(c["pv"].as_uint()) : 1;
    e.subject  = c["sub"].as_text();
    e.data     = c["data"];
    // src_uuid / dst_uuid / compression are filled by unframe() from the header.
    return e;
}

// ── wire header (routing in the clear) ──
//   [ comp:1 ][ flags:1 ][ srclen:2 BE ][ src ][ dstlen:2 BE ][ dst ]
namespace {
struct Header {
    Compression compression = Compression::None;
    std::string src, dst;
    size_t payload_offset = 0;
    bool ok = false;
};
inline uint16_t rd_be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
Header parse_header(const uint8_t* p, size_t n) {
    Header h;
    if (n < 4) return h;                       // comp + flags + srclen minimum
    h.compression = Compression(p[0]);
    if (uint8_t(h.compression) > 2) return h;  // unknown codec
    size_t i = 2;                              // skip comp + flags
    uint16_t sl = rd_be16(p + i); i += 2;
    if (i + sl > n) return h;
    h.src.assign(reinterpret_cast<const char*>(p + i), sl); i += sl;
    if (i + 2 > n) return h;
    uint16_t dl = rd_be16(p + i); i += 2;
    if (i + dl > n) return h;
    h.dst.assign(reinterpret_cast<const char*>(p + i), dl); i += dl;
    h.payload_offset = i;
    h.ok = (i < n);                            // need at least one payload byte
    return h;
}
}  // namespace

std::optional<Routing> peek_routing(const Bytes& wire) {
    Header h = parse_header(wire.data(), wire.size());
    if (!h.ok) return std::nullopt;
    return Routing{h.compression, h.src, h.dst};
}

// ── compression ──
Bytes compress(const Bytes& raw, Compression method) {
    if (method == Compression::None || raw.empty()) return raw;
    if (method == Compression::Zlib) {
        profiler::ScopedOp _("compress.zlib", raw.size(), 0);
        uLong bound = compressBound(uLong(raw.size()));
        Bytes out;
        out.resize(size_t(bound));
        uLongf out_len = bound;
        int rc = compress2(out.data(), &out_len, raw.data(), uLong(raw.size()), g_zlib_level);
        if (rc != Z_OK) return {};
        out.resize(out_len);
        return out;
    }
#ifdef UNINET_HAS_LZ4
    if (method == Compression::Lz4) {
        profiler::ScopedOp _("compress.lz4", raw.size(), 0);
        size_t bound = LZ4F_compressFrameBound(raw.size(), nullptr);
        Bytes out;
        out.resize(bound);
        size_t n = LZ4F_compressFrame(out.data(), out.size(), raw.data(), raw.size(), nullptr);
        if (LZ4F_isError(n)) return {};
        out.resize(n);
        return out;
    }
#endif
    return {};
}

Bytes decompress(const Bytes& comp, Compression method) {
    if (method == Compression::None || comp.empty()) return comp;
    if (method == Compression::Zlib) {
        profiler::ScopedOp _("decompress.zlib", comp.size(), 0);
        // Grow the output buffer until uncompress succeeds (size unknown).
        Bytes out(comp.size() * 4 + 64);
        for (int attempt = 0; attempt < 8; ++attempt) {
            uLongf out_len = uLongf(out.size());
            int rc = uncompress(out.data(), &out_len, comp.data(), uLong(comp.size()));
            if (rc == Z_OK) { out.resize(out_len); return out; }
            if (rc != Z_BUF_ERROR) return {};
            out.resize(out.size() * 2);
        }
        return {};
    }
#ifdef UNINET_HAS_LZ4
    if (method == Compression::Lz4) {
        profiler::ScopedOp _("decompress.lz4", comp.size(), 0);
        LZ4F_decompressionContext_t ctx;
        if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))) return {};
        Bytes scratch(comp.size() * 6 + 1024);
        Bytes out;
        size_t in_pos = 0;
        bool err = false;
        // Streaming decode: LZ4F_decompress may need several calls for one frame.
        for (int attempt = 0; attempt < 32; ++attempt) {
            size_t out_avail = scratch.size();
            size_t in_avail = comp.size() - in_pos;
            if (in_avail == 0 && !out.empty() && attempt > 0) break;
            size_t rc = LZ4F_decompress(ctx, scratch.data(), &out_avail,
                                        comp.data() + in_pos, &in_avail, nullptr);
            in_pos += in_avail;
            out.insert(out.end(), scratch.data(), scratch.data() + out_avail);
            if (LZ4F_isError(rc)) { err = true; break; }
            if (rc == 0 && in_pos == comp.size()) break;   // fully consumed + flushed
        }
        LZ4F_freeDecompressionContext(ctx);
        if (err) return {};
        return out;
    }
#endif
    return {};
}

// ── wire framing (clear routing header + compressed core) ──
namespace {
inline void wr_be16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
}

Bytes frame(const Envelope& e) {
    profiler::ScopedOp _("frame");
    Bytes core = encode(to_cbor(e));
    Bytes payload = (e.compression == Compression::None) ? core : compress(core, e.compression);
    Bytes wire;
    wire.reserve(payload.size() + 2 + 2 + e.src_uuid.size() + 2 + e.dst_uuid.size() + 1);
    wire.push_back(uint8_t(e.compression));   // comp
    wire.push_back(0);                          // flags (reserved)
    wr_be16(wire, uint16_t(e.src_uuid.size())); wire.insert(wire.end(), e.src_uuid.begin(), e.src_uuid.end());
    wr_be16(wire, uint16_t(e.dst_uuid.size())); wire.insert(wire.end(), e.dst_uuid.begin(), e.dst_uuid.end());
    wire.insert(wire.end(), payload.begin(), payload.end());
    _.set_bytes_in(core.size());
    _.set_bytes_out(wire.size());
    return wire;
}

std::optional<Envelope> unframe(const Bytes& wire) {
    profiler::ScopedOp _("unframe");
    Header h = parse_header(wire.data(), wire.size());
    if (!h.ok) return std::nullopt;
    Bytes payload(wire.begin() + h.payload_offset, wire.end());
    Bytes core = (h.compression == Compression::None) ? payload : decompress(payload, h.compression);
    if (core.empty()) return std::nullopt;
    bool ok = false;
    Cbor root = decode(core.data(), core.size(), &ok);
    if (!ok) return std::nullopt;
    auto env = from_cbor(root);
    if (!env) return std::nullopt;
    env->compression = h.compression;
    env->src_uuid = std::move(h.src);
    env->dst_uuid = std::move(h.dst);
    _.set_bytes_in(wire.size());
    _.set_bytes_out(core.size());
    return env;
}

}  // namespace uninet
