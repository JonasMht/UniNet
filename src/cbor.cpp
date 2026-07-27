// UniNet: CBOR codec implementation (RFC 8949). Dependency-free; definite-length
// by default with indefinite-length input accepted.
#include "uninet/cbor.h"

#include "uninet/profiler.h"

#include <cstring>
#include <unordered_set>

namespace uninet {

// ── map/array access ──
bool Cbor::has(const std::string& k) const {
    if (kind_ != Kind::Map) return false;
    for (const auto& [key, v] : map_) if (key == k) return true;
    return false;
}

const Cbor& Cbor::operator[](const std::string& k) const {
    static const Cbor null_value;
    if (kind_ != Kind::Map) return null_value;
    for (const auto& [key, v] : map_) if (key == k) return v;
    return null_value;
}

// Builder API: setting an existing key overwrites it. The scan that costs is on
// the decode path, which uses append_unchecked() plus one duplicate check instead.
Cbor& Cbor::set(const std::string& key, Cbor val) {
    if (kind_ != Kind::Map) kind_ = Kind::Map;
    for (auto& kv : map_) {
        if (kv.first == key) { kv.second = std::move(val); return *this; }
    }
    map_.emplace_back(key, std::move(val));
    return *this;
}

// ── encode ──
namespace {
inline void put_head(Bytes& out, uint8_t major, uint64_t val) {
    uint8_t ai;
    if (val < 24) {
        ai = uint8_t(val);
        out.push_back(uint8_t((major << 5) | ai));
    } else if (val <= 0xFF) {
        out.push_back(uint8_t((major << 5) | 24)); out.push_back(uint8_t(val));
    } else if (val <= 0xFFFF) {
        out.push_back(uint8_t((major << 5) | 25));
        out.push_back(uint8_t(val >> 8)); out.push_back(uint8_t(val));
    } else if (val <= 0xFFFFFFFF) {
        out.push_back(uint8_t((major << 5) | 26));
        out.push_back(uint8_t(val >> 24)); out.push_back(uint8_t(val >> 16));
        out.push_back(uint8_t(val >> 8));  out.push_back(uint8_t(val));
    } else {
        out.push_back(uint8_t((major << 5) | 27));
        for (int shift = 56; shift >= 0; shift -= 8) out.push_back(uint8_t(val >> shift));
    }
}

void encode_rec(const Cbor& c, Bytes& out) {
    switch (c.kind()) {
        case Cbor::Kind::Null:    out.push_back(0xF6); break;
        case Cbor::Kind::Bool:    out.push_back(c.as_bool() ? 0xF5 : 0xF4); break;
        case Cbor::Kind::Uint:    put_head(out, 0, c.as_uint()); break;
        case Cbor::Kind::Nint:    put_head(out, 1, uint64_t(-(c.as_int() + 1))); break;
        case Cbor::Kind::Bytes: {
            const Bytes& b = c.as_bytes();
            put_head(out, 2, b.size());
            out.insert(out.end(), b.begin(), b.end());
            break;
        }
        case Cbor::Kind::Text: {
            const std::string& s = c.as_text();
            put_head(out, 3, s.size());
            out.insert(out.end(), s.begin(), s.end());
            break;
        }
        case Cbor::Kind::Array: {
            const auto& a = c.array_items();
            put_head(out, 4, a.size());
            for (const auto& e : a) encode_rec(e, out);
            break;
        }
        case Cbor::Kind::F32Array: {
            // Bulk-write path (profiler-flagged: per-float push_back was 86% of
            // framing). Pre-size the buffer once and write directly, no per-element
            // size check / reallocation in the hot loop.
            const auto& v = c.f32_items();
            put_head(out, 4, v.size());
            const size_t base = out.size();
            out.resize(base + v.size() * 5);
            uint8_t* p = out.data() + base;
            for (size_t i = 0; i < v.size(); ++i) {
                uint32_t bits; std::memcpy(&bits, &v[i], 4);
                *p++ = 0xFA;
                *p++ = uint8_t(bits >> 24); *p++ = uint8_t(bits >> 16);
                *p++ = uint8_t(bits >> 8);  *p++ = uint8_t(bits);
            }
            break;
        }
        case Cbor::Kind::F64Array: {
            const auto& v = c.f64_items();
            put_head(out, 4, v.size());
            const size_t base = out.size();
            out.resize(base + v.size() * 9);
            uint8_t* p = out.data() + base;
            for (size_t i = 0; i < v.size(); ++i) {
                uint64_t bits; std::memcpy(&bits, &v[i], 8);
                *p++ = 0xFB;
                for (int shift = 56; shift >= 0; shift -= 8) *p++ = uint8_t(bits >> shift);
            }
            break;
        }
        case Cbor::Kind::Map: {
            const auto& m = c.map_items();
            put_head(out, 5, m.size());
            for (const auto& [k, v] : m) {
                put_head(out, 3, k.size());
                out.insert(out.end(), k.begin(), k.end());
                encode_rec(v, out);
            }
            break;
        }
        case Cbor::Kind::Float32: {
            out.push_back(uint8_t((7 << 5) | 26));
            float f = c.as_f32();
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(bits >> shift));
            break;
        }
        case Cbor::Kind::Float64: {
            out.push_back(uint8_t((7 << 5) | 27));
            double d = c.as_f64();
            uint64_t bits;
            std::memcpy(&bits, &d, 8);
            for (int shift = 56; shift >= 0; shift -= 8) out.push_back(uint8_t(bits >> shift));
            break;
        }
    }
}
}  // namespace

void encode_into(const Cbor& c, Bytes& out) {
    profiler::ScopedOp _("cbor.encode");
    out.clear();                 // keeps capacity: the point of this overload
    encode_rec(c, out);
    _.set_bytes_in(out.size());
    _.set_bytes_out(out.size());
}

Bytes encode(const Cbor& c) {
    Bytes out;
    encode_into(c, out);
    return out;
}

// ── decode ──
namespace {
// A frame arrives from the network, so every length prefix in it is attacker
// controlled and 64 bits wide. `c.i + n` and `n * 5` both wrap on 64-bit, which
// turned every bounds check below into a no-op for crafted inputs: a 9-byte
// frame declaring a 2^64-1 byte string reached the std::vector range ctor and
// aborted the process. Depth is capped for the same reason: decode_one
// recurses once per nesting level, and ~8.5k levels (which LZ4-compress to
// about 60 bytes) exhausted the stack.
constexpr int kMaxDepth = 128;

// Element counts are attacker-controlled too, and a container's count costs us a
// Cbor node (~200 bytes) per element while costing the sender one wire byte. The
// fits() checks below bound a container by the bytes actually present; this bounds
// the whole decode, because those bytes can themselves arrive compressed: 1042
// zlib bytes expand to `9A 00100000` + 1M zeros = 1M nodes = ~390 MB of nodes.
constexpr uint64_t kMaxNodes = 1u << 18;

struct Cursor { const uint8_t* p; size_t len; size_t i; bool ok; int depth; uint64_t nodes; };

// true when `count` items of `stride` bytes fit in the cursor from position i,
// computed without overflowing.
inline bool fits(const Cursor& c, uint64_t count, uint64_t stride) {
    const uint64_t avail = uint64_t(c.len - c.i);        // c.i <= c.len invariant
    if (stride != 0 && count > avail / stride) return false;
    return count * stride <= avail;
}

inline uint64_t read_be(Cursor& c, int n) {
    uint64_t v = 0;
    for (int k = 0; k < n; ++k) {
        if (c.i >= c.len) { c.ok = false; return 0; }
        v = (v << 8) | c.p[c.i++];
    }
    return v;
}

inline uint64_t read_arg(Cursor& c, uint8_t ai) {
    if (ai < 24) return ai;
    switch (ai) {
        case 24: return read_be(c, 1);
        case 25: return read_be(c, 2);
        case 26: return read_be(c, 4);
        case 27: return read_be(c, 8);
        default: c.ok = false; return 0;  // 28-30 reserved, 31 = break (handled by caller)
    }
}

// Duplicate map keys are what set() used to absorb (last one won). Detecting them
// costs a scan, and doing that scan per key is the O(k^2) the decoder was paying,
// so: linear while the map is small (the 3-key envelope core never allocates a
// hash set), hashed once it isn't.
struct KeyGuard {
    static constexpr size_t kScanLimit = 16;
    std::unordered_set<std::string> seen;

    bool is_new(const std::vector<std::pair<std::string, Cbor>>& kv, const std::string& k) {
        if (kv.size() < kScanLimit) {
            for (const auto& e : kv) if (e.first == k) return false;
            return true;
        }
        if (seen.empty()) for (const auto& e : kv) seen.insert(e.first);
        return seen.insert(k).second;
    }
};

// IEEE 754 half -> float. Half-precision is not something UniNet emits, but the
// C# peer's PeterO.Cbor picks it for any float it can represent exactly (1.0f,
// 0.5f, 2.0f...), so rejecting it dropped whole envelopes from that peer.
inline float half_to_float(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp  = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;                                   // +/-0
        } else {                                           // subnormal: renormalize
            uint32_t m = mant, shift = 0;
            while (!(m & 0x400u)) { m <<= 1; ++shift; }
            bits = sign | ((113u - shift) << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);          // inf / NaN
    } else {
        bits = sign | ((exp + 112u) << 23) | (mant << 13); // bias 127 - 15
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

Cbor decode_one(Cursor& c);

Cbor decode_one(Cursor& c) {
    if (c.i >= c.len) { c.ok = false; return Cbor::null(); }
    if (c.depth >= kMaxDepth) { c.ok = false; return Cbor::null(); }
    if (++c.nodes > kMaxNodes) { c.ok = false; return Cbor::null(); }
    struct DepthGuard { int& d; ~DepthGuard() { --d; } } _g{++c.depth};
    uint8_t ib = c.p[c.i++];
    uint8_t major = uint8_t(ib >> 5);
    uint8_t ai = uint8_t(ib & 0x1F);

    switch (major) {
        case 0: return Cbor::uint(read_arg(c, ai));
        case 1: {
            uint64_t v = read_arg(c, ai);
            if (!c.ok) return Cbor::null();
            // CBOR reaches -2^64, int64_t stops at -2^63. The subtraction below
            // wrapped for larger arguments and integer() then filed the result as
            // a Uint: -2^64 decoded as +0 with ok=1. Refuse what we cannot hold.
            if (v > uint64_t(INT64_MAX)) { c.ok = false; return Cbor::null(); }
            return Cbor::integer(int64_t(-1) - int64_t(v));
        }
        case 2: {  // byte string
            Cbor out;
            if (ai == 31) {  // indefinite -> concatenate chunks
                Bytes acc;
                for (;;) {
                    if (c.i >= c.len) { c.ok = false; return Cbor::null(); }
                    if (c.p[c.i] == 0xFF) { c.i++; break; }
                    Cbor chunk = decode_one(c);
                    if (!c.ok || !chunk.is_bytes()) { c.ok = false; return Cbor::null(); }
                    const Bytes& b = chunk.as_bytes();
                    acc.insert(acc.end(), b.begin(), b.end());
                }
                return Cbor::bytes(std::move(acc));
            }
            uint64_t n = read_arg(c, ai);
            if (!c.ok || !fits(c, n, 1)) { c.ok = false; return Cbor::null(); }
            Bytes b(c.p + c.i, c.p + c.i + n); c.i += size_t(n);
            return Cbor::bytes(std::move(b));
        }
        case 3: {  // text string
            Cbor out;
            if (ai == 31) {
                std::string acc;
                for (;;) {
                    if (c.i >= c.len) { c.ok = false; return Cbor::null(); }
                    if (c.p[c.i] == 0xFF) { c.i++; break; }
                    Cbor chunk = decode_one(c);
                    if (!c.ok || !chunk.is_text()) { c.ok = false; return Cbor::null(); }
                    acc += chunk.as_text();
                }
                return Cbor::text(std::move(acc));
            }
            uint64_t n = read_arg(c, ai);
            if (!c.ok || !fits(c, n, 1)) { c.ok = false; return Cbor::null(); }
            std::string s(reinterpret_cast<const char*>(c.p + c.i), size_t(n)); c.i += size_t(n);
            return Cbor::text(std::move(s));
        }
        case 4: {  // array
            Cbor arr = Cbor::array();
            if (ai == 31) {
                for (;;) {
                    if (c.i >= c.len) { c.ok = false; return Cbor::null(); }
                    if (c.p[c.i] == 0xFF) { c.i++; break; }
                    arr.push_back(decode_one(c));
                    if (!c.ok) return Cbor::null();
                }
                return arr;
            }
            uint64_t n = read_arg(c, ai);
            if (!c.ok) return Cbor::null();
            // Every element costs the sender at least one wire byte, so the count
            // must fit the bytes we were actually handed. Without this the count
            // alone drove the allocation: `9A 00100000` + 1M zero bytes is 1042
            // bytes of zlib and ~390 MB of Cbor nodes.
            if (!fits(c, n, 1)) { c.ok = false; return Cbor::null(); }
            // Fast path: homogeneous float32 array (each element = 0xFA + 4 BE bytes).
            if (n > 0 && fits(c, n, 5)) {
                bool all_f32 = true;
                for (uint64_t k = 0; k < n; ++k)
                    if (c.p[c.i + k * 5] != 0xFA) { all_f32 = false; break; }
                if (all_f32) {
                    std::vector<float> v; v.reserve(size_t(n));
                    for (uint64_t k = 0; k < n; ++k) {
                        const uint8_t* q = c.p + c.i + k * 5 + 1;
                        uint32_t bits = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) |
                                        (uint32_t(q[2]) << 8) | uint32_t(q[3]);
                        float f; std::memcpy(&f, &bits, 4);
                        v.push_back(f);
                    }
                    c.i += size_t(n * 5);
                    return Cbor::f32_array(std::move(v));
                }
            }
            // Fast path: homogeneous float64 array (0xFB + 8 BE bytes).
            if (n > 0 && fits(c, n, 9)) {
                bool all_f64 = true;
                for (uint64_t k = 0; k < n; ++k)
                    if (c.p[c.i + k * 9] != 0xFB) { all_f64 = false; break; }
                if (all_f64) {
                    std::vector<double> v; v.reserve(size_t(n));
                    for (uint64_t k = 0; k < n; ++k) {
                        const uint8_t* q = c.p + c.i + k * 9 + 1;
                        uint64_t bits = 0;
                        for (int b = 0; b < 8; ++b) bits = (bits << 8) | q[b];
                        double f; std::memcpy(&f, &bits, 8);
                        v.push_back(f);
                    }
                    c.i += size_t(n * 9);
                    return Cbor::f64_array(std::move(v));
                }
            }
            for (uint64_t k = 0; k < n; ++k) {
                arr.push_back(decode_one(c));
                if (!c.ok) return Cbor::null();
            }
            return arr;
        }
        case 5: {  // map
            Cbor m = Cbor::map();
            // Duplicate keys are not well-formed (RFC 8949 §5.6) and set() used to
            // hide them behind last-one-wins, which is a decoder disagreeing with
            // its peers about what a message says. Reject them.
            KeyGuard keys;
            if (ai == 31) {
                for (;;) {
                    if (c.i >= c.len) { c.ok = false; return Cbor::null(); }
                    if (c.p[c.i] == 0xFF) { c.i++; break; }
                    Cbor k = decode_one(c); if (!c.ok || !k.is_text()) { c.ok = false; return Cbor::null(); }
                    Cbor v = decode_one(c); if (!c.ok) return Cbor::null();
                    if (!keys.is_new(m.map_items(), k.as_text())) { c.ok = false; return Cbor::null(); }
                    m.append_unchecked(k.as_text(), std::move(v));
                }
                return m;
            }
            uint64_t n = read_arg(c, ai);
            if (!c.ok) return Cbor::null();
            // Two bytes minimum per entry (a key and a value), same reasoning as
            // the array above.
            if (!fits(c, n, 2)) { c.ok = false; return Cbor::null(); }
            for (uint64_t j = 0; j < n; ++j) {
                Cbor k = decode_one(c); if (!c.ok || !k.is_text()) { c.ok = false; return Cbor::null(); }
                Cbor v = decode_one(c); if (!c.ok) return Cbor::null();
                if (!keys.is_new(m.map_items(), k.as_text())) { c.ok = false; return Cbor::null(); }
                m.append_unchecked(k.as_text(), std::move(v));
            }
            return m;
        }
        case 6: {  // tag: skip the tag number, decode the content
            (void)read_arg(c, ai);
            if (!c.ok) return Cbor::null();
            return decode_one(c);
        }
        case 7: {  // simple / float / break
            if (ai == 31) { c.ok = false; return Cbor::null(); }  // stray break
            // 28-30 are reserved and carry no argument. They used to fall through
            // to the simple-value return below and decode as uint(28..30) with
            // ok=1, so a byte the spec has no meaning for became a plausible value.
            if (ai >= 28) { c.ok = false; return Cbor::null(); }
            if (ai == 20) return Cbor::boolean(false);
            if (ai == 21) return Cbor::boolean(true);
            if (ai == 22 || ai == 23) return Cbor::null();  // undefined/unused -> null
            if (ai == 24) {
                // Simple value in a following byte. It was never consumed, so the
                // byte was re-read as the next data item's head.
                uint64_t v = read_be(c, 1);
                if (!c.ok) return Cbor::null();
                if (v < 32) { c.ok = false; return Cbor::null(); }  // must have used ai<24
                return Cbor::uint(v);
            }
            if (ai == 25) {  // IEEE half, what the C# peer emits for exact floats
                if (c.i + 2 > c.len) { c.ok = false; return Cbor::null(); }
                uint16_t h = uint16_t(read_be(c, 2));
                return Cbor::f32(half_to_float(h));
            }
            if (ai == 26) {
                if (c.i + 4 > c.len) { c.ok = false; return Cbor::null(); }
                uint32_t bits = uint32_t(read_be(c, 4));
                float f; std::memcpy(&f, &bits, 4);
                return Cbor::f32(f);
            }
            if (ai == 27) {
                if (c.i + 8 > c.len) { c.ok = false; return Cbor::null(); }
                uint64_t bits = read_be(c, 8);
                double d; std::memcpy(&d, &bits, 8);
                return Cbor::f64(d);
            }
            return Cbor::uint(ai);  // simple value < 24
        }
    }
    c.ok = false;
    return Cbor::null();
}
}  // namespace

Cbor decode(const uint8_t* data, size_t len, bool* ok) {
    profiler::ScopedOp _("cbor.decode", len, len);
    if (!data || len == 0) { if (ok) *ok = false; return Cbor::null(); }
    Cursor c{data, len, 0, true, 0, 0};
    Cbor v = decode_one(c);
    if (c.i != len) c.ok = false;  // trailing garbage
    if (ok) *ok = c.ok;
    return c.ok ? v : Cbor::null();
}

Cbor Cbor::decode(const Bytes& b, bool* ok) {
    return uninet::decode(b.data(), b.size(), ok);
}

}  // namespace uninet
