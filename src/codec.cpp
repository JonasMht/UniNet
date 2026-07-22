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

// ── envelope <-> cbor ──
Cbor to_cbor(const Envelope& e) {
    return Cbor::map()
        .set("pv", Cbor::uint(e.protocol_version))
        .set("cp", Cbor::uint(uint8_t(e.compression)))
        .set("src", Cbor::text(e.src_uuid))
        .set("dst", Cbor::text(e.dst_uuid))
        .set("sub", Cbor::text(e.subject))
        .set("data", e.data);
}

std::optional<Envelope> from_cbor(const Cbor& c) {
    if (!c.is_map() || !c.has("src") || !c.has("sub") || !c.has("data"))
        return std::nullopt;
    Envelope e;
    e.protocol_version = c.has("pv") ? uint16_t(c["pv"].as_uint()) : 1;
    e.compression = c.has("cp") ? Compression(uint8_t(c["cp"].as_uint())) : Compression::None;
    if (uint8_t(e.compression) > 2) return std::nullopt;
    e.src_uuid = c["src"].as_text();
    e.dst_uuid = c.has("dst") ? c["dst"].as_text() : std::string{};
    e.subject  = c["sub"].as_text();
    e.data     = c["data"];
    return e;
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

// ── wire framing ──
Bytes frame(const Envelope& e) {
    Bytes cbor = encode(to_cbor(e));
    Bytes payload = (e.compression == Compression::None) ? cbor : compress(cbor, e.compression);
    Bytes wire;
    wire.reserve(payload.size() + 1);
    wire.push_back(uint8_t(e.compression));
    wire.insert(wire.end(), payload.begin(), payload.end());
    return wire;
}

std::optional<Envelope> unframe(const Bytes& wire) {
    if (wire.size() < 2) return std::nullopt;   // 1 header byte + >=1 payload byte
    Compression method = Compression(wire[0]);
    if (uint8_t(method) > 2) return std::nullopt;
    Bytes payload(wire.begin() + 1, wire.end());
    Bytes cbor = (method == Compression::None) ? payload : decompress(payload, method);
    if (cbor.empty()) return std::nullopt;
    bool ok = false;
    Cbor root = decode(cbor.data(), cbor.size(), &ok);
    if (!ok) return std::nullopt;
    return from_cbor(root);
}

}  // namespace uninet
