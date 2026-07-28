// UniNet: envelope framing + compression implementation.
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

// How far a frame may expand relative to its own size before we call it a bomb
// rather than a payload. The LZ4 frame format tops out near 255x and deflate near
// 1032x, so this accepts anything a real compressor can emit while stopping a
// tiny hostile frame from sizing a huge buffer.
constexpr size_t kMaxExpansion = 2048;

// Growing a decompression buffer is the one place where a remote peer picks our
// allocation size, so every resize on that path goes through here: it reports
// failure instead of letting length_error/bad_alloc escape (decompress_into and
// unframe_into are treated as noexcept by their callers).
bool try_resize(Bytes& b, size_t n) {
    if (n > MAX_DECOMPRESSED_BYTES) return false;
    try {
        b.resize(n);
    } catch (...) {
        b.clear();
        return false;
    }
    return true;
}
}  // namespace

void set_compression_level(int level) {
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    g_zlib_level = level;
}

// ── envelope core <-> cbor (subject/data/version only; routing is in the header) ──
Cbor to_cbor(const Envelope& e) {
    Cbor m = Cbor::map()
        .set("pv", Cbor::uint(e.protocol_version))
        .set("sub", Cbor::text(e.subject))
        .set("data", e.data);
    // Only when there is one, so a message that needs no deduplication costs
    // no extra bytes.
    if (!e.mid.empty()) m.set("mid", Cbor::text(e.mid));
    return m;
}

std::optional<Envelope> from_cbor(const Cbor& c) {
    if (!c.is_map() || !c.has("sub") || !c.has("data"))
        return std::nullopt;
    Envelope e;
    e.protocol_version = c.has("pv") ? uint16_t(c["pv"].as_uint()) : 1;
    e.subject  = c["sub"].as_text();
    e.data     = c["data"];
    if (c.has("mid")) e.mid = c["mid"].as_text();
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
        // reallocations after the first message. The growth is bounded by the
        // policy ceiling rather than by a retry count: the old 8-doublings budget
        // both let a frame ask for 512x its own size (a bomb) and silently dropped
        // legitimate payloads that compressed better than that (deflate reaches
        // 1032x, and the sparse safety maps get close).
        const size_t first = (n < MAX_DECOMPRESSED_BYTES / 4) ? n * 4 + 64 : MAX_DECOMPRESSED_BYTES;
        if (out.size() < first && !try_resize(out, first)) { out.clear(); return false; }
        for (;;) {
            uLongf out_len = uLongf(out.size());
            int rc = uncompress(out.data(), &out_len, comp, uLong(n));
            if (rc == Z_OK) { out.resize(out_len); return true; }
            if (rc != Z_BUF_ERROR) { out.clear(); return false; }
            if (out.size() >= MAX_DECOMPRESSED_BYTES) { out.clear(); return false; }
            const size_t grown = out.size() < MAX_DECOMPRESSED_BYTES / 2 ? out.size() * 2
                                                                         : MAX_DECOMPRESSED_BYTES;
            if (!try_resize(out, grown)) { out.clear(); return false; }
        }
    }
#ifdef UNINET_HAS_LZ4
    if (method == Compression::Lz4) {
        profiler::ScopedOp _("decompress.lz4", n, 0);
        // The frame header carries the content size when the compressor knew it,
        // so one sized decompress replaces the old grow-and-retry streaming loop
        // and its per-call scratch allocation.
        //
        // One context per thread, reused across frames, and OWNED, so it is freed
        // when the thread exits. The bare thread_local pointer this replaces was
        // never freed: ~131 KB leaked per thread that ever received an LZ4 frame
        // (LeakSanitizer).
        struct Ctx {
            LZ4F_decompressionContext_t c = nullptr;
            Ctx()  { if (LZ4F_isError(LZ4F_createDecompressionContext(&c, LZ4F_VERSION))) c = nullptr; }
            ~Ctx() { if (c) LZ4F_freeDecompressionContext(c); }
            Ctx(const Ctx&) = delete;
            Ctx& operator=(const Ctx&) = delete;
        };
        static thread_local Ctx ctx;
        if (!ctx.c) { out.clear(); return false; }
        LZ4F_resetDecompressionContext(ctx.c);

        size_t hdr = n;
        LZ4F_frameInfo_t info{};
        size_t rc = LZ4F_getFrameInfo(ctx.c, &info, comp, &hdr);
        if (LZ4F_isError(rc)) { out.clear(); return false; }

        // info.contentSize is the SENDER's claim, not a measurement: a 34-byte
        // frame declaring 2^64-1 bytes of content sized the buffer straight into
        // std::length_error, i.e. an abort on the receive path. Believe it only up
        // to the policy ceiling and to a sane expansion of the bytes actually in
        // hand; anything real that needs more grows into it in the loop below.
        const size_t limit = (n < MAX_DECOMPRESSED_BYTES / kMaxExpansion)
                                 ? n * kMaxExpansion + 1024 : MAX_DECOMPRESSED_BYTES;
        const uint64_t want = info.contentSize ? info.contentSize : (uint64_t(n) * 6 + 1024);
        const size_t cap = want > uint64_t(limit) ? limit : size_t(want);
        if (out.size() < cap && !try_resize(out, cap)) { out.clear(); return false; }

        size_t in_pos = hdr, produced = 0;
        bool complete = false;
        for (int attempt = 0; attempt < 64; ++attempt) {
            size_t out_avail = out.size() - produced;
            size_t in_avail = n - in_pos;
            rc = LZ4F_decompress(ctx.c, out.data() + produced, &out_avail,
                                 comp + in_pos, &in_avail, nullptr);
            if (LZ4F_isError(rc)) { out.clear(); return false; }
            in_pos += in_avail;
            produced += out_avail;
            if (rc == 0) { complete = true; break; }  // frame ended cleanly
            if (in_pos >= n && out_avail == 0) break; // no progress possible
            if (produced == out.size()) {
                const size_t grown = out.size() < limit / 2 ? out.size() * 2 : limit;
                if (grown <= out.size() || !try_resize(out, grown)) { out.clear(); return false; }
            }
        }
        // A nonzero rc means LZ4 is still waiting for bytes we do not have, and
        // running out of attempts means the same. Both used to fall through to
        // `return true`, so a truncated frame decoded into a plausible-looking
        // envelope instead of being dropped.
        if (!complete) { out.clear(); return false; }
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
    // The header's length fields are 16 bits. A longer uuid used to be truncated
    // by the uint16_t cast, which does not produce a frame addressed to the wrong
    // peer. It produces a frame whose header no longer parses, so a unicast
    // silently became malformed garbage. Refuse to build it instead.
    if (e.src_uuid.size() > 0xFFFF || e.dst_uuid.size() > 0xFFFF) { wire.clear(); return; }
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

    // Uncompressed frames decode straight out of the caller's buffer: the old
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
