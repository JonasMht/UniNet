// UniNet — JSON <-> Cbor bridge. See include/uninet/json.h.
//
// Dependency-free, like the CBOR codec next to it, and written to the same
// standard: this parses text that arrived from the network, so it is bounded,
// non-throwing, and refuses malformed input rather than guessing at it.
#include "uninet/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace uninet {
namespace {

// Same limit the CBOR decoder uses. Deep nesting is the cheapest way to turn a
// small payload into a stack overflow, and no real message is 128 deep.
constexpr int kMaxDepth = 128;

struct Parser {
    const char* p;
    const char* end;
    bool ok = true;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool eat(char c) {
        skip_ws();
        if (p < end && *p == c) { ++p; return true; }
        return false;
    }
    bool literal(const char* s) {
        const size_t n = std::strlen(s);
        if (size_t(end - p) < n || std::strncmp(p, s, n) != 0) return false;
        p += n;
        return true;
    }

    // Encode one code point as UTF-8. JSON \u escapes are UTF-16, so the caller
    // pairs surrogates before calling this.
    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(uint32_t& out) {
        if (end - p < 4) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = p[i];
            out <<= 4;
            if (c >= '0' && c <= '9')      out |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') out |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= uint32_t(c - 'A' + 10);
            else return false;
        }
        p += 4;
        return true;
    }

    bool parse_string(std::string& out) {
        if (!eat('"')) return false;
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (c != '\\') {
                // Raw control characters are illegal in JSON strings, and
                // letting them through would put terminal escapes into any log
                // that prints this value.
                if (static_cast<unsigned char>(c) < 0x20) return false;
                out.push_back(c);
                continue;
            }
            if (p >= end) return false;
            switch (*p++) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate: a low one must follow, or the text is
                        // not valid UTF-16 and we refuse it rather than emit
                        // mojibake.
                        uint32_t lo = 0;
                        if (end - p < 2 || p[0] != '\\' || p[1] != 'u') return false;
                        p += 2;
                        if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;   // lone low surrogate
                    }
                    utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    Cbor parse_value(int depth) {
        if (depth > kMaxDepth) { ok = false; return Cbor::null(); }
        skip_ws();
        if (p >= end) { ok = false; return Cbor::null(); }

        switch (*p) {
            case 'n': if (literal("null"))  return Cbor::null();
                      ok = false; return Cbor::null();
            case 't': if (literal("true"))  return Cbor::boolean(true);
                      ok = false; return Cbor::null();
            case 'f': if (literal("false")) return Cbor::boolean(false);
                      ok = false; return Cbor::null();
            case '"': {
                std::string s;
                if (!parse_string(s)) { ok = false; return Cbor::null(); }
                return Cbor::text(std::move(s));
            }
            case '[': {
                ++p;
                Cbor arr = Cbor::array();
                skip_ws();
                if (eat(']')) return arr;
                for (;;) {
                    arr.push_back(parse_value(depth + 1));
                    if (!ok) return Cbor::null();
                    skip_ws();
                    if (eat(',')) continue;
                    if (eat(']')) return arr;
                    ok = false;
                    return Cbor::null();
                }
            }
            case '{': {
                ++p;
                Cbor obj = Cbor::map();
                skip_ws();
                if (eat('}')) return obj;
                for (;;) {
                    skip_ws();
                    std::string key;
                    if (!parse_string(key)) { ok = false; return Cbor::null(); }
                    if (!eat(':')) { ok = false; return Cbor::null(); }
                    Cbor v = parse_value(depth + 1);
                    if (!ok) return Cbor::null();
                    obj.set(key, std::move(v));
                    skip_ws();
                    if (eat(',')) continue;
                    if (eat('}')) return obj;
                    ok = false;
                    return Cbor::null();
                }
            }
            default: break;
        }

        // Number. Integers stay integers so they survive the round-trip as CBOR
        // uints rather than becoming floats that print as "4.0".
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        bool is_float = false;
        while (p < end) {
            const char c = *p;
            if (c >= '0' && c <= '9') { ++p; continue; }
            if (c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
                is_float = true;
                ++p;
                continue;
            }
            break;
        }
        if (p == start) { ok = false; return Cbor::null(); }

        const std::string num(start, size_t(p - start));
        if (!is_float) {
            errno = 0;
            char* endp = nullptr;
            if (num[0] == '-') {
                const long long v = std::strtoll(num.c_str(), &endp, 10);
                if (errno == 0 && endp == num.c_str() + num.size())
                    return Cbor::integer(int64_t(v));
            } else {
                const unsigned long long v = std::strtoull(num.c_str(), &endp, 10);
                if (errno == 0 && endp == num.c_str() + num.size())
                    return Cbor::uint(uint64_t(v));
            }
            // Out of integer range: fall through and keep it as a double rather
            // than losing the value entirely.
        }
        char* endp = nullptr;
        const double d = std::strtod(num.c_str(), &endp);
        if (endp != num.c_str() + num.size()) { ok = false; return Cbor::null(); }
        return Cbor::f64(d);
    }
};

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64(const Bytes& in, std::string& out) {
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
    }
    if (i + 1 == in.size()) {
        const uint32_t v = uint32_t(in[i]) << 16;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.append("==");
    } else if (i + 2 == in.size()) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back('=');
    }
}

void escape(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
                    out.append(buf);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

void number(double d, std::string& out) {
    if (!std::isfinite(d)) { out.append("null"); return; }   // JSON has no NaN/Inf
    char buf[40];
    // 17 significant digits round-trips an IEEE double exactly.
    std::snprintf(buf, sizeof buf, "%.17g", d);
    out.append(buf);
}

void write(const Cbor& v, std::string& out, int indent, int depth) {
    const bool pretty = indent > 0;
    const std::string nl   = pretty ? "\n" : "";
    const std::string pad  = pretty ? std::string(size_t(indent) * size_t(depth + 1), ' ') : "";
    const std::string pad0 = pretty ? std::string(size_t(indent) * size_t(depth), ' ') : "";
    const std::string sep  = pretty ? ": " : ":";

    switch (v.kind()) {
        case Cbor::Kind::Null:    out.append("null"); break;
        case Cbor::Kind::Bool:    out.append(v.as_bool() ? "true" : "false"); break;
        case Cbor::Kind::Uint:    out.append(std::to_string(v.as_uint())); break;
        case Cbor::Kind::Nint:    out.append(std::to_string(v.as_int())); break;
        case Cbor::Kind::Float32:
        case Cbor::Kind::Float64: number(v.as_f64(), out); break;
        case Cbor::Kind::Text:    escape(v.as_text(), out); break;
        case Cbor::Kind::Bytes: {
            std::string b64;
            base64(v.as_bytes(), b64);
            escape(b64, out);
            break;
        }
        case Cbor::Kind::Array: {
            const auto& items = v.array_items();
            if (items.empty()) { out.append("[]"); break; }
            out.append("[").append(nl);
            for (size_t i = 0; i < items.size(); ++i) {
                out.append(pad);
                write(items[i], out, indent, depth + 1);
                if (i + 1 < items.size()) out.append(",");
                out.append(nl);
            }
            out.append(pad0).append("]");
            break;
        }
        case Cbor::Kind::F32Array:
        case Cbor::Kind::F64Array: {
            // Mesh payloads land here — thousands of numbers. They stay on one
            // line whatever the indent, because a 12288-line array helps nobody.
            const size_t n = v.size();
            out.append("[");
            for (size_t i = 0; i < n; ++i) {
                if (i) out.append(",");
                number(v.kind() == Cbor::Kind::F32Array ? double(v.f32_items()[i])
                                                        : v.f64_items()[i], out);
            }
            out.append("]");
            break;
        }
        case Cbor::Kind::Map: {
            const auto& items = v.map_items();
            if (items.empty()) { out.append("{}"); break; }
            out.append("{").append(nl);
            for (size_t i = 0; i < items.size(); ++i) {
                out.append(pad);
                escape(items[i].first, out);
                out.append(sep);
                write(items[i].second, out, indent, depth + 1);
                if (i + 1 < items.size()) out.append(",");
                out.append(nl);
            }
            out.append(pad0).append("}");
            break;
        }
    }
}

}  // namespace

Cbor from_json(const std::string& text, bool* ok) {
    Parser ps{text.data(), text.data() + text.size(), true};
    Cbor v = ps.parse_value(0);
    if (ps.ok) {
        ps.skip_ws();
        if (ps.p != ps.end) ps.ok = false;   // trailing garbage is an error, not a hint
    }
    if (ok) *ok = ps.ok;
    return ps.ok ? v : Cbor::null();
}

std::string to_json(const Cbor& value, int indent) {
    std::string out;
    write(value, out, indent, 0);
    return out;
}

}  // namespace uninet
