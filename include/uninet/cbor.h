// UniNet — minimal, dependency-free CBOR (RFC 8949) value type + codec. CBOR is
// UniNet's default compact wire codec for the envelope and message payloads, built
// directly on the standard library — no third-party CBOR library (cf. UniVox's
// hand-rolled .npz on zlib). Supports the subset real messages use: uint/nint,
// byte & text strings, arrays, maps (insertion-ordered), bool/null, float32/64.
//
// Why own it: the ThermoNav peers each pull a different CBOR lib (PeterO.Cbor,
// nlohmann::json::to_cbor, cbor2) and hand-mirror the schema across them. One
// codec, one source of truth.
#pragma once

#include "uninet/types.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace uninet {

class Cbor {
public:
    enum class Kind {
        Null, Bool, Uint, Nint, Bytes, Text, Array, Map, Float32, Float64,
        F32Array, F64Array   // contiguous homogeneous float arrays (fast path)
    };

    Cbor() = default;

    // ── factories ──
    static Cbor null()                                  { Cbor c; c.kind_ = Kind::Null; return c; }
    static Cbor boolean(bool b)                         { Cbor c; c.kind_ = Kind::Bool;  c.b_ = b; return c; }
    static Cbor uint(uint64_t v)                        { Cbor c; c.kind_ = Kind::Uint;  c.u_ = v; return c; }
    static Cbor integer(int64_t v) {
        Cbor c;
        if (v >= 0) { c.kind_ = Kind::Uint; c.u_ = uint64_t(v); }
        else        { c.kind_ = Kind::Nint; c.i_ = v; }
        return c;
    }
    static Cbor bytes(Bytes b)                          { Cbor c; c.kind_ = Kind::Bytes; c.s_ = std::move(b); return c; }
    static Cbor text(std::string s)                     { Cbor c; c.kind_ = Kind::Text;  c.str_ = std::move(s); return c; }
    static Cbor f32(float f)                            { Cbor c; c.kind_ = Kind::Float32; c.d_ = double(f); return c; }
    static Cbor f64(double f)                           { Cbor c; c.kind_ = Kind::Float64; c.d_ = f; return c; }
    static Cbor array()                                 { Cbor c; c.kind_ = Kind::Array; return c; }
    static Cbor map()                                   { Cbor c; c.kind_ = Kind::Map;   return c; }
    static Cbor array(std::vector<Cbor> a)              { Cbor c; c.kind_ = Kind::Array; c.arr_ = std::move(a); return c; }
    // Common case: a homogeneous float array (mesh points / transforms / maps).
    // Stored contiguously and encoded/decoded in a tight loop (no per-element node),
    // since float arrays are the heavy payload (mesh points, safety textures, maps).
    static Cbor f32_array(const float* p, size_t n) {
        Cbor c; c.kind_ = Kind::F32Array; c.f32_.assign(p, p + n); return c;
    }
    static Cbor f32_array(std::vector<float> v) {
        Cbor c; c.kind_ = Kind::F32Array; c.f32_ = std::move(v); return c;
    }
    static Cbor f64_array(const double* p, size_t n) {
        Cbor c; c.kind_ = Kind::F64Array; c.f64_.assign(p, p + n); return c;
    }
    static Cbor f64_array(std::vector<double> v) {
        Cbor c; c.kind_ = Kind::F64Array; c.f64_ = std::move(v); return c;
    }

    // ── inspection ──
    Kind kind() const { return kind_; }
    bool is_null()   const { return kind_ == Kind::Null; }
    bool is_bool()   const { return kind_ == Kind::Bool; }
    bool is_uint()   const { return kind_ == Kind::Uint; }
    bool is_int()    const { return kind_ == Kind::Uint || kind_ == Kind::Nint; }
    bool is_number() const { return kind_ == Kind::Uint || kind_ == Kind::Nint ||
                                    kind_ == Kind::Float32 || kind_ == Kind::Float64; }
    bool is_text()   const { return kind_ == Kind::Text; }
    bool is_bytes()  const { return kind_ == Kind::Bytes; }
    bool is_array()  const { return kind_ == Kind::Array; }
    bool is_map()    const { return kind_ == Kind::Map; }

    uint64_t as_uint() const { return kind_ == Kind::Uint ? u_ : uint64_t(i_); }
    int64_t  as_int()  const { return kind_ == Kind::Uint ? int64_t(u_) : i_; }
    bool     as_bool() const { return b_; }
    double   as_f64()  const { return d_; }
    float    as_f32()  const { return float(d_); }
    const std::string& as_text()  const { return str_; }
    const Bytes&       as_bytes() const { return s_; }

    size_t size() const {
        return kind_ == Kind::Array ? arr_.size()
             : (kind_ == Kind::Map ? map_.size()
             : ((kind_ == Kind::F32Array) ? f32_.size()
             : ((kind_ == Kind::F64Array) ? f64_.size() : 0)));
    }

    // Array index. size() counts the floats of an F32Array/F64Array, and the
    // decoder turns any all-float array on the wire into one, so the obvious
    // consumer loop `for (i < v.size()) v[i]` was indexing the empty arr_ — an
    // out-of-bounds read driven by remote input. Those kinds are boxed on demand
    // here; bulk consumers still take f32_items()/f64_items() and pay nothing.
    // Out of range (or a non-array) yields a null value, never UB.
    //
    // The boxed element lives in a per-thread slot, so the reference is valid
    // only until the next index of a float array on this thread.
    const Cbor& operator[](size_t i) const {
        if (kind_ == Kind::Array)    return i < arr_.size() ? arr_[i] : null_value();
        if (kind_ == Kind::F32Array) return i < f32_.size() ? box(f32(f32_[i])) : null_value();
        if (kind_ == Kind::F64Array) return i < f64_.size() ? box(f64(f64_[i])) : null_value();
        return null_value();
    }
    const std::vector<Cbor>& array_items() const { return arr_; }
    const std::vector<float>&  f32_items() const { return f32_; }
    const std::vector<double>& f64_items() const { return f64_; }

    // Map access (text keys). has(); operator[] returns a static null if missing.
    bool has(const std::string& k) const;
    const Cbor& at(const std::string& k) const { return (*this)[k]; }
    const Cbor& operator[](const std::string& k) const;
    const Cbor& operator[](const char* k) const { return (*this)[std::string(k)]; }
    const std::vector<std::pair<std::string, Cbor>>& map_items() const { return map_; }

    // Map/array mutation (builder style: returns *this).
    Cbor& set(const std::string& key, Cbor val);
    Cbor& push_back(Cbor v) { arr_.push_back(std::move(v)); return *this; }

    // Append a map entry WITHOUT set()'s "is this key already here" scan. Only for
    // callers that already know the key is new: the decoder, which does its own
    // duplicate detection once per map instead of once per key (set()'s scan made
    // decoding a k-key map O(k^2) — 4.14 s for one 68 KB frame).
    Cbor& append_unchecked(std::string key, Cbor val) {
        kind_ = Kind::Map;
        map_.emplace_back(std::move(key), std::move(val));
        return *this;
    }

    // Round-trip.
    friend Bytes encode(const Cbor& c);
    friend Cbor  decode(const uint8_t* data, size_t len, bool* ok);
    static Cbor  decode(const Bytes& b, bool* ok = nullptr);

private:
    static const Cbor& null_value() { static const Cbor n; return n; }
    static const Cbor& box(Cbor v) {
        static thread_local Cbor slot;
        slot = std::move(v);
        return slot;
    }

    Kind kind_ = Kind::Null;
    uint64_t u_ = 0;
    int64_t  i_ = 0;
    bool     b_ = false;
    double   d_ = 0.0;
    std::string str_;                                 // Text
    Bytes       s_;                                   // Bytes
    std::vector<float>  f32_;                         // F32Array (contiguous)
    std::vector<double> f64_;                         // F64Array (contiguous)
    std::vector<Cbor> arr_;                           // Array
    std::vector<std::pair<std::string, Cbor>> map_;   // Map (insertion-ordered)
};

// Serialize a Cbor value to canonical definite-length CBOR bytes.
Bytes encode(const Cbor& c);
// Same, into a caller-owned buffer (cleared, capacity retained). Hot paths reuse
// one buffer across calls instead of allocating+freeing a fresh Bytes per message
// — above glibc's 128 KiB mmap threshold that allocation is an mmap/munmap pair.
void encode_into(const Cbor& c, Bytes& out);
// Parse CBOR bytes. Sets *ok=false (if ok != nullptr) on any malformed input and
// returns Cbor::null(). Indefinite-length containers are accepted.
Cbor decode(const uint8_t* data, size_t len, bool* ok = nullptr);

}  // namespace uninet
