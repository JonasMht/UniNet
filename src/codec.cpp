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
void compress_into(const uint8_t* raw, size_t n, Compression method, Bytes& out) {
    if (method == Compression::None || n == 0) {
        out.assign(raw, raw + n);
        return;
    }
    if (method == Compression::Zlib) {
        profiler::ScopedOp _("compress.zlib", n, 0);
        uLong bound = compressBound(uLong(n));
        out.resize(size_t(bound));            // keeps capacity across calls
        uLongf out_len = bound;
        int rc = compress2(out.data(), &out_len, raw, uLong(n), g_zlib_level);
        if (rc != Z_OK) { out.clear(); return; }
        out.resize(out_len);
        return;
    }
#ifdef UNINET_HAS_LZ4
    if (method == Compression::Lz4) {
        profiler::ScopedOp _("compress.lz4", n, 0);
        size_t bound = LZ4F_compressFrameBound(n, nullptr);
        out.resize(bound);
        size_t w = LZ4F_compressFrame(out.data(), out.size(), raw, n, nullptr);
        if (LZ4F_isError(w)) { out.clear(); return; }
        out.resize(w);
        return;
    }
#endif
    out.clear();
}

bool decompress_into(const uint8_t* comp, size_t n, Compression method, Bytes& out) {
    if (method == Compression::None || n == 0) {
        out.assign(comp, comp + n);
        return true;
    }
    if (method == Compression::Zlib) {
        profiler::ScopedOp _("decompress.zlib", n, 0);
        // Output size isn't on the wire, so grow until it fits. `out` is reused
        // across calls, so a steady stream of similar frames converges to zero
        // reallocations after the first message.
        if (out.size() < n * 4 + 64) out.resize(n * 4 + 64);
        for (int attempt = 0; attempt < 8; ++attempt) {
            uLongf out_len = uLongf(out.size());
            int rc = uncompress(out.data(), &out_len, comp, uLong(n));
            if (rc == Z_OK) { out.resize(out_len); return true; }
            if (rc != Z_BUF_ERROR) { out.clear(); return false; }
            out.resize(out.size() * 2);
        }
        out.clear();
        return false;
    }
#ifdef UNINET_HAS_LZ4
    if (method == Compression::Lz4) {
        profiler::ScopedOp _("decompress.lz4", n, 0);
        // The frame header carries the content size when the compressor knew it
        // (LZ4F_compressFrame does), so one sized decompress replaces the old
        // grow-and-retry streaming loop and its per-call scratch allocation.
        static thread_local LZ4F_decompressionContext_t ctx = nullptr;
        if (!ctx && LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION))) {
            ctx = nullptr;
            out.clear();
            return false;
        }
        LZ4F_resetDecompressionContext(ctx);

        size_t hdr = n;
        LZ4F_frameInfo_t info{};
        size_t rc = LZ4F_getFrameInfo(ctx, &info, comp, &hdr);
        if (LZ4F_isError(rc)) { out.clear(); return false; }

        size_t cap = info.contentSize ? size_t(info.contentSize) : (n * 6 + 1024);
        if (out.size() < cap) out.resize(cap);

        size_t in_pos = hdr, produced = 0;
        for (int attempt = 0; attempt < 32; ++attempt) {
            size_t out_avail = out.size() - produced;
            size_t in_avail = n - in_pos;
            rc = LZ4F_decompress(ctx, out.data() + produced, &out_avail,
                                 comp + in_pos, &in_avail, nullptr);
            if (LZ4F_isError(rc)) { out.clear(); return false; }
            in_pos += in_avail;
            produced += out_avail;
            if (rc == 0) break;                       // frame complete
            if (in_pos >= n && out_avail == 0) break; // no progress possible
            if (produced == out.size()) out.resize(out.size() * 2);
        }
        out.resize(produced);
        return true;
    }
#endif
    out.clear();
    return false;
}

Bytes compress(const Bytes& raw, Compression method) {
    if (method == Compression::None || raw.empty()) return raw;
    Bytes out;
    compress_into(raw.data(), raw.size(), method, out);
    return out;
}

Bytes decompress(const Bytes& comp, Compression method) {
    if (method == Compression::None || comp.empty()) return comp;
    Bytes out;
    if (!decompress_into(comp.data(), comp.size(), method, out)) return {};
    return out;
}

// ── wire framing (clear routing header + compressed core) ──
namespace {
inline void wr_be16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
}

void frame_into(const Envelope& e, Bytes& wire, Scratch& scratch) {
    profiler::ScopedOp _("frame");
    encode_into(to_cbor(e), scratch.core);

    const uint8_t* payload = scratch.core.data();
    size_t payload_n = scratch.core.size();
    if (e.compression != Compression::None) {
        compress_into(scratch.core.data(), scratch.core.size(), e.compression, scratch.payload);
        payload = scratch.payload.data();
        payload_n = scratch.payload.size();
    }

    const size_t hdr = 2 + 2 + e.src_uuid.size() + 2 + e.dst_uuid.size();
    wire.clear();
    wire.reserve(hdr + payload_n);
    wire.push_back(uint8_t(e.compression));   // comp
    wire.push_back(0);                        // flags (reserved)
    wr_be16(wire, uint16_t(e.src_uuid.size())); wire.insert(wire.end(), e.src_uuid.begin(), e.src_uuid.end());
    wr_be16(wire, uint16_t(e.dst_uuid.size())); wire.insert(wire.end(), e.dst_uuid.begin(), e.dst_uuid.end());
    wire.insert(wire.end(), payload, payload + payload_n);
    _.set_bytes_in(scratch.core.size());
    _.set_bytes_out(wire.size());
}

std::optional<Envelope> unframe_into(const uint8_t* wire, size_t n, Scratch& scratch) {
    profiler::ScopedOp _("unframe");
    Header h = parse_header(wire, n);
    if (!h.ok) return std::nullopt;

    // Uncompressed frames decode straight out of the caller's buffer — the old
    // path copied the whole payload out first purely to get a Bytes to hand to
    // decompress().
    const uint8_t* core = wire + h.payload_offset;
    size_t core_n = n - h.payload_offset;
    if (h.compression != Compression::None) {
        if (!decompress_into(core, core_n, h.compression, scratch.core)) return std::nullopt;
        core = scratch.core.data();
        core_n = scratch.core.size();
    }
    if (core_n == 0) return std::nullopt;

    bool ok = false;
    Cbor root = decode(core, core_n, &ok);
    if (!ok) return std::nullopt;
    auto env = from_cbor(root);
    if (!env) return std::nullopt;
    env->compression = h.compression;
    env->src_uuid = std::move(h.src);
    env->dst_uuid = std::move(h.dst);
    _.set_bytes_in(n);
    _.set_bytes_out(core_n);
    return env;
}

Bytes frame(const Envelope& e) {
    Scratch s;
    Bytes wire;
    frame_into(e, wire, s);
    return wire;
}

std::optional<Envelope> unframe(const Bytes& wire) {
    Scratch s;
    return unframe_into(wire.data(), wire.size(), s);
}

}  // namespace uninet
