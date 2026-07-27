// UniNet: JSON <-> Cbor bridge. See include/uninet/json.h.
//
// Dependency-free, like the CBOR codec next to it, and written to the same
// standard: this parses text that arrived from the network, so it is bounded,
// non-throwing, and refuses malformed input rather than guessing at it.
#include "uninet/json.h"

#include <cerrno>
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
        //
        // Scanned to JSON's actual grammar rather than "digits and punctuation":
        // the loose version accepted 0123, +5, 1. and .5, all invalid JSON, and
        // produced a value for them.
        const char* start = p;
        if (p < end && *p == '-') ++p;                    // no leading '+' in JSON
        const char* int_start = p;
        if (p < end && *p == '0') {
            ++p;                                         // a leading zero stands alone
        } else {
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p == int_start) { ok = false; return Cbor::null(); }   // ".5", "-", "e5"
        bool is_float = false;
        if (p < end && *p == '.') {
            ++p;
            const char* frac = p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
            if (p == frac) { ok = false; return Cbor::null(); }     // "1."
            is_float = true;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            const char* exp = p;
            while (p < end && *p >= '0' && *p <= '9') ++p;
            if (p == exp) { ok = false; return Cbor::null(); }      // "1e", "1e+"
            is_float = true;
        }

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
        errno = 0;
        const double d = std::strtod(num.c_str(), &endp);
        if (endp != num.c_str() + num.size()) { ok = false; return Cbor::null(); }
        // errno was checked on the integer path but not here, so "1e999" parsed
        // as +infinity and was happily published; to_json then rendered it as
        // null, so the value silently became nothing at the far end.
        if (errno == ERANGE && (d > 1.0 || d < -1.0)) { ok = false; return Cbor::null(); }
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

// Validate a UTF-8 sequence starting at s[i]. Returns its length, or 0 when the
// bytes are not valid UTF-8.
size_t utf8_len(const std::string& s, size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t need;
    uint32_t cp;
    if (c < 0x80)                   return 1;
    else if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07u; }
    else                            return 0;          // continuation or 5-byte form
    if (i + need >= s.size() + 0 && i + need > s.size() - 1) return 0;
    for (size_t k = 1; k <= need; ++k) {
        const unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    // Reject overlong forms, surrogates and out-of-range code points: all of
    // them would produce JSON no strict parser accepts.
    if (need == 1 && cp < 0x80)    return 0;
    if (need == 2 && cp < 0x800)   return 0;
    if (need == 3 && cp < 0x10000) return 0;
    if (cp > 0x10FFFF)             return 0;
    if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
    return need + 1;
}

void escape(const std::string& s, std::string& out) {
    // A CBOR text value is not UTF-8-validated on decode (the wire format allows
    // any bytes), so passing it through verbatim could emit JSON that no parser
    // accepts, with no signal. Invalid bytes become U+FFFD instead.
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char u = static_cast<unsigned char>(s[i]);
        if (u >= 0x80) {
            const size_t n = utf8_len(s, i);
            if (n == 0) {
                out.append("\ufffd");     // replacement character
                ++i;
            } else {
                out.append(s, i, n);
                i += n;
            }
            continue;
        }
        const char c = s[i++];
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
                break;
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
    // The parser caps nesting; the writer did not, so a programmatically built
    // deep value overflowed the stack on the way out.
    if (depth > kMaxDepth) { out.append("null"); return; }
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
            // Mesh payloads land here: thousands of numbers. They stay on one
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
