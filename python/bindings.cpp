// UniNet: Python bindings.
//
// The goal is that Python code looks like Python:
//
//     net = uninet.join("Slicer Viewer", role="viewer")
//     net.subscribe("domain.>", lambda m: print(m.subject, m.data))
//     net.publish("domain.D1", {"code": "update", "points": [1.0, 2.0]})
//
// A dict goes in and a dict comes out. The CBOR layer is there when you want it
// (uninet.Cbor) but nothing requires you to touch it, a Python value converts
// to the same wire bytes a C++ Cbor builder or a C# JSON string would produce,
// which is the whole point of owning one codec.
//
// Two things this file has to get right, because the previous version did not:
//
//  1. THE GIL. Callbacks arrive on Zyre's network thread. Every entry into
//     Python needs the GIL, and not only at call time: Node copies its handler
//     list before dispatching, so a captured py::function would have its
//     refcount touched on that thread with no GIL held. Handlers are therefore
//     held through a shared_ptr whose deleter takes the GIL, and the
//     std::function copies only the shared_ptr.
//
//  2. LIFETIME. Node borrows a raw Transport*, so Python must be stopped from
//     collecting a transport a live Node still points at (py::keep_alive).
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "uninet/blob.h"
#include "uninet/cbor.h"
#include "uninet/codec.h"
#include "uninet/json.h"
#include "uninet/loopback.h"
#include "uninet/node.h"
#include "uninet/peer.h"
#include "uninet/profiler.h"
#include "uninet/diagnostics.h"
#include "uninet/session.h"
#include "uninet/zyre_transport.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace py = pybind11;
using namespace uninet;

namespace {

// ── Python value <-> Cbor ─────────────────────────────────────────────────
// Explicit conversion rather than a pybind11 type_caster: a caster would claim
// every Python object silently, and a wrong guess here becomes a wrong byte on
// the wire.

Cbor py_to_cbor(const py::handle& obj, int depth = 0);

// A list of FLOATS becomes a contiguous float array: the fast path the codec
// exists for. Mesh vertices arrive as an ordinary Python list and still cost one
// bulk write rather than a Cbor node per coordinate.
//
// Integers deliberately do NOT take this path. A list of ints is data whose
// int-ness matters: mesh triangle indices, an array shape, a voxel count: and
// silently returning it as [256.0, 256.0, 256.0] breaks the caller in a way that
// is very hard to trace back to here.
bool all_floats(const py::sequence& seq) {
    if (py::len(seq) == 0) return false;   // an empty list has no numeric intent
    for (const auto& item : seq) {
        // bool is a subclass of int in Python, and neither is a float.
        if (!py::isinstance<py::float_>(item)) return false;
    }
    return true;
}

Cbor py_to_cbor(const py::handle& obj, int depth) {
    // The decoder caps nesting at 128; refuse to build what it could not read
    // back, rather than failing at the far end where it looks like a bug there.
    if (depth > 100) throw py::value_error("uninet: value nested too deeply");

    if (obj.is_none())                  return Cbor::null();
    if (py::isinstance<py::bool_>(obj)) return Cbor::boolean(obj.cast<bool>());
    if (py::isinstance<py::int_>(obj)) {
        // Python integers are unbounded; the wire format's are not.
        try { return Cbor::integer(int64_t(obj.cast<long long>())); }
        catch (const py::cast_error&) {
            try { return Cbor::uint(obj.cast<unsigned long long>()); }
            catch (const py::cast_error&) {
                throw py::value_error("uninet: integer too large for the wire format");
            }
        }
    }
    if (py::isinstance<py::float_>(obj)) return Cbor::f64(obj.cast<double>());
    if (py::isinstance<py::str>(obj))    return Cbor::text(obj.cast<std::string>());
    if (py::isinstance<py::bytes>(obj) || py::isinstance<py::bytearray>(obj)) {
        const auto s = obj.cast<std::string>();
        return Cbor::bytes(Bytes(s.begin(), s.end()));
    }
    if (py::isinstance<Cbor>(obj))       return obj.cast<Cbor>();

    if (py::isinstance<py::dict>(obj)) {
        Cbor m = Cbor::map();
        for (const auto& kv : obj.cast<py::dict>()) {
            if (!py::isinstance<py::str>(kv.first))
                throw py::type_error("uninet: message keys must be strings");
            m.set(kv.first.cast<std::string>(), py_to_cbor(kv.second, depth + 1));
        }
        return m;
    }
    if (py::isinstance<py::list>(obj) || py::isinstance<py::tuple>(obj)) {
        auto seq = obj.cast<py::sequence>();
        if (all_floats(seq)) {
            std::vector<double> v;
            v.reserve(py::len(seq));
            for (const auto& item : seq) v.push_back(item.cast<double>());
            return Cbor::f64_array(std::move(v));
        }
        Cbor a = Cbor::array();
        for (const auto& item : seq) a.push_back(py_to_cbor(item, depth + 1));
        return a;
    }

    // Anything exposing the buffer protocol (numpy arrays, array.array,
    // memoryview) is read directly rather than via .tolist(). Going through a
    // Python list materialised one object per element, cost ~15x, and widened
    // float32 to float64 on the wire.
    if (py::isinstance<py::buffer>(obj)) {
        const py::buffer_info info = obj.cast<py::buffer>().request();
        // A strided view must NOT be read as a flat block: `arr[::2]` would
        // transmit the wrong elements with a plausible byte count and nothing
        // would look wrong at any layer. Fall back to the element-wise path.
        bool contiguous = info.ndim == 0;
        if (!contiguous && info.ndim >= 1) {
            ssize_t expected = info.itemsize;
            contiguous = true;
            for (ssize_t d = info.ndim - 1; d >= 0; --d) {
                if (info.strides[d] != expected) { contiguous = false; break; }
                expected *= info.shape[d];
            }
        }
        // ndim <= 1 only. A contiguous 2-D array took the fast path and was
        // FLATTENED, losing its shape, while the same array in Fortran order
        // fell through to tolist() and kept its nesting: one logical value with
        // two different wire shapes depending on memory order.
        if (contiguous && info.size > 0 && info.ndim <= 1) {
            const size_t n = size_t(info.size);
            if (info.format == py::format_descriptor<float>::format())
                return Cbor::f32_array(static_cast<const float*>(info.ptr), n);
            if (info.format == py::format_descriptor<double>::format())
                return Cbor::f64_array(static_cast<const double*>(info.ptr), n);
        }
    }
    // Everything else array-like: correct, just not on the fast path.
    if (py::hasattr(obj, "tolist")) return py_to_cbor(obj.attr("tolist")(), depth + 1);

    throw py::type_error("uninet: cannot send a value of type '" +
                         std::string(py::str(py::type::handle_of(obj).attr("__name__"))) + "'");
}

py::object cbor_to_py(const Cbor& c) {
    switch (c.kind()) {
        case Cbor::Kind::Null:    return py::none();
        case Cbor::Kind::Bool:    return py::bool_(c.as_bool());
        case Cbor::Kind::Uint:    return py::int_(c.as_uint());
        case Cbor::Kind::Nint:    return py::int_(c.as_int());
        case Cbor::Kind::Float32:
        case Cbor::Kind::Float64: return py::float_(c.as_f64());
        case Cbor::Kind::Text:    return py::str(c.as_text());
        case Cbor::Kind::Bytes: {
            const Bytes& b = c.as_bytes();
            return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
        }
        case Cbor::Kind::Array: {
            py::list l;
            for (const auto& item : c.array_items()) l.append(cbor_to_py(item));
            return std::move(l);
        }
        case Cbor::Kind::F32Array: {
            py::list l;
            for (float f : c.f32_items()) l.append(py::float_(double(f)));
            return std::move(l);
        }
        case Cbor::Kind::F64Array: {
            py::list l;
            for (double d : c.f64_items()) l.append(py::float_(d));
            return std::move(l);
        }
        case Cbor::Kind::Map: {
            py::dict d;
            for (const auto& kv : c.map_items()) d[py::str(kv.first)] = cbor_to_py(kv.second);
            return std::move(d);
        }
    }
    return py::none();
}

// ── holding a Python callable safely across threads ───────────────────────
// The returned shared_ptr copies freely on any thread without touching a Python
// refcount; only the final destruction re-enters Python, and it takes the GIL.
std::shared_ptr<py::function> hold(py::function fn) {
    return std::shared_ptr<py::function>(new py::function(std::move(fn)),
                                         [](py::function* f) {
                                             py::gil_scoped_acquire gil;
                                             delete f;
                                         });
}

// Call a Python handler from a network thread. A raising handler must not
// unwind into C++ and from there into Zyre's C frames, so it is reported and
// swallowed the way a callback exception is in any event loop.
template <typename Arg>
void call_guarded(const std::shared_ptr<py::function>& fn, const char* what, const Arg& arg) {
    py::gil_scoped_acquire gil;
    try {
        (*fn)(arg);
    } catch (const py::error_already_set& e) {
        std::fprintf(stderr, "uninet: exception in %s handler: %s\n", what, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uninet: exception in %s handler: %s\n", what, e.what());
    } catch (...) {
        std::fprintf(stderr, "uninet: unknown exception in %s handler\n", what);
    }
}

// Same, for the two-argument handlers (progress, failure, completion).
template <typename A, typename B>
void call_guarded2(const std::shared_ptr<py::function>& fn, const char* what,
                   const A& a, const B& b) {
    py::gil_scoped_acquire gil;
    try {
        (*fn)(a, b);
    } catch (const py::error_already_set& e) {
        std::fprintf(stderr, "uninet: exception in %s handler: %s\n", what, e.what());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uninet: exception in %s handler: %s\n", what, e.what());
    } catch (...) {
        std::fprintf(stderr, "uninet: unknown exception in %s handler\n", what);
    }
}

// What a subscriber receives: a small value object rather than the raw
// Envelope, so `msg.data` is already a dict and `msg.json()` is already text.
struct Message {
    std::string subject;
    std::string src;
    Cbor        data;
};

}  // namespace

PYBIND11_MODULE(_uninet, m) {
    m.doc() = "UniNet: brokerless peer-to-peer messaging with discovery built in.";

    py::enum_<Compression>(m, "Compression")
        // NOT "None": that is a Python keyword, so the attribute would be
        // unreachable. NONE/ZLIB/LZ4 read the same in every language anyway.
        .value("NONE", Compression::None)
        .value("ZLIB", Compression::Zlib)
        .value("LZ4",  Compression::Lz4);

    // ── Cbor: available, never required ──
    py::class_<Cbor> cbor(m, "Cbor",
        "The wire value type. You rarely need it: publish() takes a dict.");
    py::enum_<Cbor::Kind>(cbor, "Kind")
        .value("Null", Cbor::Kind::Null).value("Bool", Cbor::Kind::Bool)
        .value("Uint", Cbor::Kind::Uint).value("Nint", Cbor::Kind::Nint)
        .value("Bytes", Cbor::Kind::Bytes).value("Text", Cbor::Kind::Text)
        .value("Array", Cbor::Kind::Array).value("Map", Cbor::Kind::Map)
        .value("Float32", Cbor::Kind::Float32).value("Float64", Cbor::Kind::Float64)
        .value("F32Array", Cbor::Kind::F32Array).value("F64Array", Cbor::Kind::F64Array);
    cbor.def(py::init<>())
        .def_static("from_value", [](const py::object& o) { return py_to_cbor(o); },
                    py::arg("value"), "Build from a dict / list / scalar.")
        .def("to_value", [](const Cbor& c) { return cbor_to_py(c); },
             "Convert to a dict / list / scalar.")
        .def_static("null", &Cbor::null)
        .def_static("boolean", &Cbor::boolean)
        .def_static("uint", &Cbor::uint)
        .def_static("integer", &Cbor::integer)
        .def_static("text", [](const std::string& s) { return Cbor::text(s); })
        .def_static("f32", &Cbor::f32)
        .def_static("f64", &Cbor::f64)
        .def_static("map", []() { return Cbor::map(); })
        .def_static("array", []() { return Cbor::array(); })
        .def_static("f32_array", [](const std::vector<float>& v) { return Cbor::f32_array(v); })
        .def_static("f64_array", [](const std::vector<double>& v) { return Cbor::f64_array(v); })
        .def("kind", &Cbor::kind)
        .def("is_null", &Cbor::is_null).def("is_map", &Cbor::is_map)
        .def("is_array", &Cbor::is_array).def("is_text", &Cbor::is_text)
        .def("is_number", &Cbor::is_number)
        // The C++ accessors are documented as unchecked: each reads its own
        // field whatever the kind actually is, so as_f64() on a Uint returned
        // 0.0 and as_text() on a number returned "". In C++ that is a
        // performance choice the caller can see; from Python it is a silently
        // wrong value, and `is_number()` invites exactly that mistake.
        .def("as_bool", [](const Cbor& c) {
            if (c.kind() != Cbor::Kind::Bool)
                throw py::type_error("uninet: this value is not a bool");
            return c.as_bool();
        })
        .def("as_uint", [](const Cbor& c) -> uint64_t {
            switch (c.kind()) {
                case Cbor::Kind::Uint:    return c.as_uint();
                case Cbor::Kind::Nint:    throw py::value_error("uninet: value is negative");
                case Cbor::Kind::Float32:
                case Cbor::Kind::Float64: return uint64_t(c.as_f64());
                default: throw py::type_error("uninet: this value is not a number");
            }
        })
        .def("as_int", [](const Cbor& c) -> int64_t {
            switch (c.kind()) {
                case Cbor::Kind::Uint:    return int64_t(c.as_uint());
                case Cbor::Kind::Nint:    return c.as_int();
                case Cbor::Kind::Float32:
                case Cbor::Kind::Float64: return int64_t(c.as_f64());
                default: throw py::type_error("uninet: this value is not a number");
            }
        })
        .def("as_f64", [](const Cbor& c) -> double {
            switch (c.kind()) {
                case Cbor::Kind::Uint:    return double(c.as_uint());
                case Cbor::Kind::Nint:    return double(c.as_int());
                case Cbor::Kind::Float32:
                case Cbor::Kind::Float64: return c.as_f64();
                default: throw py::type_error("uninet: this value is not a number");
            }
        })
        .def("as_text", [](const Cbor& c) -> const std::string& {
            if (!c.is_text()) throw py::type_error("uninet: this value is not text");
            return c.as_text();
        })
        .def("as_bytes", [](const Cbor& c) {
            if (!c.is_bytes()) throw py::type_error("uninet: this value is not bytes");
            const Bytes& b = c.as_bytes();
            return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
        })
        // Both accept either float-array kind and convert. f32_items() on a
        // value that really was a float array returned [] whenever the decoder
        // had chosen F64, which it does for most wire data.
        .def("f32_items", [](const Cbor& c) {
            std::vector<float> out;
            if (c.kind() == Cbor::Kind::F32Array) return c.f32_items();
            if (c.kind() == Cbor::Kind::F64Array) {
                out.reserve(c.f64_items().size());
                for (double d : c.f64_items()) out.push_back(float(d));
                return out;
            }
            throw py::type_error("uninet: this value is not a float array");
        })
        .def("f64_items", [](const Cbor& c) {
            std::vector<double> out;
            if (c.kind() == Cbor::Kind::F64Array) return c.f64_items();
            if (c.kind() == Cbor::Kind::F32Array) {
                out.reserve(c.f32_items().size());
                for (float f : c.f32_items()) out.push_back(double(f));
                return out;
            }
            throw py::type_error("uninet: this value is not a float array");
        })
        .def("has", &Cbor::has)
        // Both refuse a mismatched kind. push_back() on a non-array left kind_
        // alone, so the elements were accepted and then silently vanished at
        // encode time; set() on an array overwrote the kind and dropped the
        // elements. Returning *this keeps the builder chainable.
        .def("set", [](Cbor& c, const std::string& k, const py::object& v) -> Cbor& {
            if (!c.is_map())
                throw py::type_error("uninet: set() needs a map (use Cbor.map())");
            c.set(k, py_to_cbor(v));
            return c;
        }, py::arg("key"), py::arg("value"), py::return_value_policy::reference_internal)
        .def("append", [](Cbor& c, const py::object& v) -> Cbor& {
            if (!c.is_array())
                throw py::type_error("uninet: append() needs an array (use Cbor.array())");
            c.push_back(py_to_cbor(v));
            return c;
        }, py::return_value_policy::reference_internal)
        .def("__len__", [](const Cbor& c) -> size_t {
            // Cbor::size() covers containers only, so len() on text or bytes
            // was 0 rather than the obvious length.
            if (c.is_text())  return c.as_text().size();
            if (c.is_bytes()) return c.as_bytes().size();
            return c.size();
        })
        .def("__contains__", &Cbor::has)
        .def("__getitem__", [](const Cbor& c, const std::string& k) {
            // Returning a null Cbor made "key missing" and "key present and
            // null" the same answer. Use .has(k) or .get(k) to probe.
            if (!c.is_map()) throw py::type_error("uninet: this value is not a map");
            if (!c.has(k)) throw py::key_error(k);
            return c[k];
        })
        .def("get", [](const Cbor& c, const std::string& k, const py::object& fallback) {
            return (c.is_map() && c.has(k)) ? py::cast(c[k]) : fallback;
        }, py::arg("key"), py::arg("default") = py::none(),
           "The value at `key`, or `default` when it is absent.")
        .def("__getitem__", [](const Cbor& c, size_t i) {
            // The C++ operator[] is documented as unchecked; from Python an
            // out-of-range index must raise, not read past the end.
            if (i >= c.size()) throw py::index_error("index out of range");
            return c[i];
        })
        .def("keys", [](const Cbor& c) {
            if (!c.is_map()) throw py::type_error("uninet: this value is not a map");
            py::list out;
            for (const auto& kv : c.map_items()) out.append(py::str(kv.first));
            return out;
        }, "The map's keys, in insertion order.")
        .def("items", [](const Cbor& c) {
            if (!c.is_map()) throw py::type_error("uninet: this value is not a map");
            py::list out;
            for (const auto& kv : c.map_items())
                out.append(py::make_tuple(py::str(kv.first), kv.second));
            return out;
        }, "(key, Cbor) pairs, in insertion order.")
        .def("values", [](const Cbor& c) {
            // F32Array/F64Array are what the decoder produces for ANY all-float
            // wire array, so testing is_array() alone made a genuinely received
            // float array non-iterable while len() and [i] both worked on it.
            py::list out;
            if (c.is_map()) {
                for (const auto& kv : c.map_items()) out.append(kv.second);
            } else if (c.is_array() || c.kind() == Cbor::Kind::F32Array ||
                       c.kind() == Cbor::Kind::F64Array) {
                for (size_t i = 0; i < c.size(); ++i) out.append(c[i]);
            } else {
                throw py::type_error("uninet: this value is not a map or array");
            }
            return out;
        }, "Values of a map, or elements of an array, without converting them.")
        .def("__iter__", [](const Cbor& c) {
            // A map yields its keys, matching dict; an array yields its
            // elements. Without this, inspecting a Cbor meant converting the
            // whole thing with to_value() and losing the Cbor layer.
            py::list out;
            if (c.is_map()) {
                for (const auto& kv : c.map_items()) out.append(py::str(kv.first));
            } else if (c.is_array() || c.kind() == Cbor::Kind::F32Array ||
                       c.kind() == Cbor::Kind::F64Array) {
                for (size_t i = 0; i < c.size(); ++i) out.append(c[i]);
            } else {
                throw py::type_error("uninet: this value is not iterable");
            }
            return py::iter(out);
        })
        .def("__repr__", [](const Cbor& c) { return "<uninet.Cbor " + to_json(c) + ">"; });

    m.def("encode", [](const py::object& v) {
        const Bytes b = encode(py_to_cbor(v));
        return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
    }, py::arg("value"), "Serialize a Python value to CBOR bytes.");

    m.def("decode", [](const py::buffer& buf) {
        const py::buffer_info info = buf.request();
        // info.size counts ELEMENTS; a memoryview cast to 'Q' would otherwise
        // decode one eighth of the payload.
        const size_t nbytes = size_t(info.size) * size_t(info.itemsize);
        bool ok = false;
        Cbor c = decode(static_cast<const uint8_t*>(info.ptr), nbytes, &ok);
        // Returning null for corrupt input made a decode failure indistinguishable
        // from a legitimate null value; raise instead.
        if (!ok) throw py::value_error("uninet: malformed CBOR");
        return cbor_to_py(c);
    }, py::arg("data"), "Parse CBOR bytes into a Python value.");

    m.def("to_json", [](const py::object& v, int indent) {
        return to_json(py_to_cbor(v), indent);
    }, py::arg("value"), py::arg("indent") = 0,
       "Render a value as JSON: the same text a C++ or C# peer would produce.");

    m.def("from_json", [](const std::string& text) {
        bool ok = false;
        Cbor c = from_json(text, &ok);
        if (!ok) throw py::value_error("uninet: malformed JSON");
        return cbor_to_py(c);
    }, py::arg("text"), "Parse JSON text into a Python value.");

    // ── Peer ──
    py::class_<Peer>(m, "Peer", "Another device on the network.")
        .def_readonly("uuid", &Peer::uuid, "Address for a private message.")
        .def_readonly("name", &Peer::name)
        .def_readonly("address", &Peer::address)
        .def_readonly("headers", &Peer::headers)
        .def_property_readonly("role", &Peer::role)
        .def_property_readonly("app", &Peer::app)
        .def_property_readonly("host", &Peer::host, "Machine hostname the peer runs on.")
        .def_property_readonly("endpoint", &Peer::endpoint, "IP address, without the port.")
        .def("header", &Peer::header, py::arg("key"))
        .def("describe", &Peer::describe)
        .def("__repr__", [](const Peer& p) { return "<uninet.Peer " + p.describe() + ">"; });

    // ── Message ──
    py::class_<Message>(m, "Message", "One received message.")
        .def_readonly("subject", &Message::subject)
        .def_readonly("src", &Message::src, "uuid of the sender.")
        .def_property_readonly("data", [](const Message& msg) { return cbor_to_py(msg.data); },
                               "Payload as a dict / list / scalar.")
        .def_property_readonly("cbor", [](const Message& msg) { return msg.data; })
        .def("json", [](const Message& msg, int indent) { return to_json(msg.data, indent); },
             py::arg("indent") = 0)
        .def("__repr__", [](const Message& msg) {
            return "<uninet.Message " + msg.subject + " " + to_json(msg.data) + ">";
        });

    py::class_<Envelope>(m, "Envelope")
        .def_readonly("subject", &Envelope::subject)
        .def_readonly("src_uuid", &Envelope::src_uuid)
        .def_readonly("dst_uuid", &Envelope::dst_uuid)
        .def_property_readonly("data", [](const Envelope& e) { return cbor_to_py(e.data); });

    // ── transports (advanced; Session covers the common case) ──
    py::class_<Transport>(m, "Transport");

    py::class_<LoopbackTransport, Transport>(m, "LoopbackTransport",
        "In-process bus. Deterministic, no network, for tests.")
        .def(py::init<>())
        .def("connect", &LoopbackTransport::connect)
        .def("disconnect", &LoopbackTransport::disconnect)
        .def("connected", &LoopbackTransport::connected);

    py::class_<ZyreConfig>(m, "ZyreConfig")
        .def(py::init<>())
        .def_readwrite("realm", &ZyreConfig::realm)
        .def_readwrite("port", &ZyreConfig::port)
        .def_readwrite("iface", &ZyreConfig::iface)
        .def_readwrite("gossip_bind", &ZyreConfig::gossip_bind)
        .def_readwrite("gossip_connect", &ZyreConfig::gossip_connect)
        .def_readwrite("endpoint", &ZyreConfig::endpoint)
        .def_readwrite("advertised_endpoint", &ZyreConfig::advertised_endpoint)
        .def_readwrite("evasive_ms", &ZyreConfig::evasive_ms,
                       "Milliseconds of silence before a peer is pinged.")
        .def_readwrite("expired_ms", &ZyreConfig::expired_ms,
                       "Milliseconds of silence before a peer is declared gone.")
        .def_readwrite("headers", &ZyreConfig::headers);

    py::class_<ZyreTransport, Transport>(m, "ZyreTransport",
        "Brokerless peer-to-peer transport with discovery built in.")
        .def(py::init<std::string, ZyreConfig>(),
             py::arg("name"), py::arg("config") = ZyreConfig{})
        // Blocking: the interpreter must not freeze while the network starts.
        .def("connect", &ZyreTransport::connect, py::call_guard<py::gil_scoped_release>())
        .def("disconnect", &ZyreTransport::disconnect, py::call_guard<py::gil_scoped_release>())
        .def("connected", &ZyreTransport::connected)
        .def("peers", &ZyreTransport::peers)
        .def("uuid", &ZyreTransport::uuid)
        .def("last_error", &ZyreTransport::last_error)
        .def("unsubscribe", &ZyreTransport::unsubscribe, py::arg("subject"))
        .def("set_header", &ZyreTransport::set_header, py::arg("key"), py::arg("value"),
             "Advertise a key/value. Must be called before connect(); returns "
             "False and sets last_error() afterwards, when it cannot take effect.")
        .def("node_name", &ZyreTransport::node_name)
        .def("can_address", &ZyreTransport::can_address);

    py::class_<Node>(m, "Node")
        // keep_alive<1,3>: the transport (arg 3) must outlive the Node (arg 1).
        // Without it, passing a temporary transport is a use-after-free on the
        // very next call.
        .def(py::init<std::string, Transport*, Compression>(),
             py::arg("name"), py::arg("transport"),
             py::arg("compression") = DEFAULT_COMPRESSION,
             py::keep_alive<1, 3>())
        .def("uuid", &Node::uuid)
        .def("name", &Node::name)
        .def("connect", &Node::connect, py::call_guard<py::gil_scoped_release>())
        .def("connected", &Node::connected)
        .def("retry_connect", &Node::retry_connect,
             py::arg("attempts"), py::arg("base_sleep_s") = 0.1,
             py::call_guard<py::gil_scoped_release>())
        .def("publish", [](Node& n, const std::string& subject, const py::object& data,
                           const std::string& dst) {
            Cbor c = py_to_cbor(data);          // convert while we hold the GIL
            py::gil_scoped_release unlock;      // send without it
            return n.publish(subject, std::move(c), dst);
        }, py::arg("subject"), py::arg("data"), py::arg("dst") = "")
        .def("subscribe", [](Node& n, const std::string& subject, py::function cb) {
            auto held = hold(std::move(cb));
            n.subscribe(subject, [held](const Envelope& env) {
                call_guarded(held, "subscribe", Message{env.subject, env.src_uuid, env.data});
            });
        }, py::arg("subject"), py::arg("handler"));

    // ── Session: the one-call API ──
    py::class_<SessionConfig>(m, "SessionConfig")
        .def(py::init<>())
        .def_readwrite("role", &SessionConfig::role)
        .def_readwrite("app", &SessionConfig::app)
        .def_readwrite("realm", &SessionConfig::realm)
        .def_readwrite("port", &SessionConfig::port)
        .def_readwrite("iface", &SessionConfig::iface)
        .def_readwrite("gossip_bind", &SessionConfig::gossip_bind)
        .def_readwrite("gossip_connect", &SessionConfig::gossip_connect)
        .def_readwrite("endpoint", &SessionConfig::endpoint)
        .def_readwrite("advertised_endpoint", &SessionConfig::advertised_endpoint)
        .def_readwrite("compression", &SessionConfig::compression)
        .def_readwrite("headers", &SessionConfig::headers);

    py::class_<Session>(m, "Session", "A device on the network. Created by uninet.join().")
        .def("publish", [](Session& s, const std::string& subject, const py::object& data,
                           const std::string& dst) {
            Cbor c = py_to_cbor(data);
            py::gil_scoped_release unlock;
            // Returning the result matters: without it a publish during a
            // network outage is indistinguishable from a delivered one.
            return s.publish(subject, std::move(c), dst);
        }, py::arg("subject"), py::arg("data"), py::arg("dst") = "",
           "Send to everyone, or to one peer's uuid. False if it could not be sent.")
        .def("publish_json", [](Session& s, const std::string& subject,
                                const std::string& json, const std::string& dst) {
            py::gil_scoped_release unlock;
            return s.publish_json(subject, json, dst);
        }, py::arg("subject"), py::arg("json"), py::arg("dst") = "")
        .def("subscribe", [](Session& s, const std::string& subject, py::function cb) {
            auto held = hold(std::move(cb));
            s.subscribe(subject, [held](const Envelope& env) {
                call_guarded(held, "subscribe", Message{env.subject, env.src_uuid, env.data});
            });
        }, py::arg("subject"), py::arg("handler"),
           "Receive messages. A subject ending in '>' matches everything below it.")
        .def("peers", &Session::peers, "Every device currently on the network.")
        .def("on_peer_found", [](Session& s, py::function cb) {
            auto held = hold(std::move(cb));
            s.on_peer_found([held](const Peer& p) { call_guarded(held, "on_peer_found", p); });
        }, py::arg("handler"))
        .def("on_peer_lost", [](Session& s, py::function cb) {
            auto held = hold(std::move(cb));
            s.on_peer_lost([held](const Peer& p) { call_guarded(held, "on_peer_lost", p); });
        }, py::arg("handler"))
        .def("connected", &Session::connected)
        .def("close", &Session::close, py::call_guard<py::gil_scoped_release>(),
             "Leave the network. Idempotent.")
        .def("open", &Session::open)
        .def("name", &Session::name)
        .def("uuid", &Session::uuid, "This device's address on the network.")
        .def("describe", &Session::describe, "One plain sentence about the connection.")
        .def("last_error", &Session::last_error,
             "Why the last operation failed. Empty when healthy.")
        .def("node", &Session::node, py::return_value_policy::reference_internal,
             "The underlying Node. Raises once the session is closed.")
        .def("transport", &Session::transport, py::return_value_policy::reference_internal,
             "The underlying transport. Raises once the session is closed.")
        .def("__repr__", [](const Session& s) { return "<uninet.Session " + s.describe() + ">"; });

    // ── Blob: large payloads (files, volumes, meshes) ──
    py::class_<BlobInfo>(m, "BlobInfo", "Describes one large transfer.")
        .def_readonly("id", &BlobInfo::id)
        .def_readonly("name", &BlobInfo::name)
        .def_readonly("src", &BlobInfo::src, "uuid of the sender.")
        .def_readonly("size", &BlobInfo::size, "Total bytes.")
        .def_property_readonly("meta", [](const BlobInfo& i) { return cbor_to_py(i.meta); },
                               "Whatever the sender attached: array shape, dtype, case id...")
        .def("__repr__", [](const BlobInfo& i) {
            return "<uninet.BlobInfo " + i.name + " " + std::to_string(i.size) + "B>";
        });

    py::class_<BlobConfig>(m, "BlobConfig")
        .def(py::init<>())
        .def_readwrite("chunk_bytes", &BlobConfig::chunk_bytes)
        .def_readwrite("max_blob_bytes", &BlobConfig::max_blob_bytes)
        .def_readwrite("max_total_bytes", &BlobConfig::max_total_bytes)
        .def_readwrite("max_concurrent", &BlobConfig::max_concurrent)
        .def_property("stall_timeout_s",
            [](const BlobConfig& c) { return double(c.stall_timeout.count()); },
            [](BlobConfig& c, double v) {
                c.stall_timeout = std::chrono::seconds(static_cast<long long>(v));
            }, "Seconds without a chunk before a transfer is abandoned.");

    py::class_<Blob>(m, "Blob",
        "Streams payloads too large for one message: files, volumes, meshes.")
        // keep_alive<1,2>: Blob holds a Session&, so the session must outlive it.
        .def(py::init<Session&, std::string, BlobConfig>(),
             py::arg("session"), py::arg("subject"), py::arg("config") = BlobConfig{},
             py::keep_alive<1, 2>())
        .def("send", [](Blob& b, const std::string& name, const py::buffer& buf,
                        const py::object& meta, const std::string& dst) {
            const py::buffer_info info = buf.request();
            // Reject a strided view. Reading it as a flat block would send the
            // WRONG ELEMENTS with a correct-looking byte count: `arr[::2]`
            // transmitted arr[0..n/2] and nothing looked wrong anywhere.
            ssize_t expected = info.itemsize;
            for (ssize_t d = info.ndim - 1; d >= 0; --d) {
                if (info.strides[d] != expected)
                    throw py::value_error(
                        "uninet: the buffer is not contiguous, so its bytes are not "
                        "the data you mean. Pass numpy.ascontiguousarray(x).");
                expected *= info.shape[d];
            }
            Cbor m2 = meta.is_none() ? Cbor::null() : py_to_cbor(meta);
            const auto* p = static_cast<const uint8_t*>(info.ptr);
            const size_t n = size_t(info.size) * size_t(info.itemsize);
            py::gil_scoped_release unlock;
            return b.send(name, p, n, std::move(m2), dst);
        }, py::arg("name"), py::arg("data"), py::arg("meta") = py::none(),
           py::arg("dst") = "",
           "Send raw bytes, or any contiguous buffer such as a numpy array.\n"
           "Returns the transfer id, or '' if it could not start.")
        .def("send_file", [](Blob& b, const std::string& path, const py::object& meta,
                             const std::string& dst, const std::string& name) {
            Cbor m2 = meta.is_none() ? Cbor::null() : py_to_cbor(meta);
            py::gil_scoped_release unlock;
            return b.send_file(path, std::move(m2), dst, name);
        }, py::arg("path"), py::arg("meta") = py::none(), py::arg("dst") = "",
           py::arg("name") = "")
        .def("on_received", [](Blob& b, py::function cb) {
            auto held = hold(std::move(cb));
            b.on_received([held](const BlobInfo& info, const Bytes& data) {
                // The GIL must be held BEFORE constructing py::bytes: building it
                // at the call site allocated a Python object on the network
                // thread with no GIL, which segfaults.
                py::gil_scoped_acquire gil;
                try {
                    (*held)(info, py::bytes(reinterpret_cast<const char*>(data.data()),
                                            data.size()));
                } catch (const py::error_already_set& e) {
                    std::fprintf(stderr, "uninet: exception in on_received handler: %s\n",
                                 e.what());
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "uninet: exception in on_received handler: %s\n",
                                 e.what());
                } catch (...) {
                    std::fprintf(stderr, "uninet: unknown exception in on_received handler\n");
                }
            });
        }, py::arg("handler"), "handler(info, data: bytes) when a transfer completes.")
        .def("on_progress", [](Blob& b, py::function cb) {
            auto held = hold(std::move(cb));
            b.on_progress([held](const BlobInfo& info, size_t done) {
                // call_guarded reports to stderr; a bare catch(...) lost the
                // exception entirely, with no way to know a handler was broken.
                call_guarded2(held, "on_progress", info, done);
            });
        }, py::arg("handler"), "handler(info, bytes_so_far) as chunks arrive.")
        .def("on_failed", [](Blob& b, py::function cb) {
            auto held = hold(std::move(cb));
            b.on_failed([held](const BlobInfo& info, const std::string& why) {
                call_guarded2(held, "on_failed", info, why);
            });
        }, py::arg("handler"), "handler(info, reason) when a transfer is abandoned.")
        .def("incoming_count", &Blob::incoming_count)
        .def("sweep", &Blob::sweep);

    m.def("_join", [](const std::string& name, SessionConfig cfg) {
        py::gil_scoped_release unlock;
        return Session::join(name, std::move(cfg));
    }, py::arg("name"), py::arg("config"));

    // ── profiler ──
    m.def("profiler_enable", &profiler::enable, py::arg("on") = true);
    m.def("profiler_report", &profiler::report);
    m.def("profiler_reset", &profiler::reset);

    // ── build info ──
    m.attr("PROTOCOL_VERSION") = int(CURRENT_PROTOCOL_VERSION);
#ifdef UNINET_HAS_LZ4
    m.attr("HAS_LZ4") = true;
#else
    m.attr("HAS_LZ4") = false;
#endif
    m.def("zyre_version", &zyre_version_string);
    m.def("local_hostname", &local_hostname,
          "This machine's hostname, as advertised to peers.");
    m.def("local_interfaces",
          []() {
              py::list out;
              for (const auto& i : local_interfaces()) {
                  py::dict d;
                  d["name"]      = i.name;
                  d["address"]   = i.address;
                  d["broadcast"] = i.broadcast;
                  d["netmask"]   = i.netmask;
                  out.append(std::move(d));
              }
              return out;
          },
          "Every usable IPv4 network on this machine, as dicts with name, "
          "address, broadcast and netmask.\n\n"
          "Discovery binds ONE of these. On a machine with Wi-Fi, a wired "
          "connection, a VPN and container bridges, the default may not be the "
          "one the other device is on, and nothing reports that. Use this to "
          "show the user a choice, and pass the name as join(iface=...).");
    m.def("diagnostics", &diagnostics,
          "What UniNet is doing right now, as text: version, compression tiers, "
          "every network on this machine and which one discovery chose, and every "
          "live session with its identity, peers and reconnect count.\n\n"
          "This is the thing to paste into a bug report. Nearly every \"it cannot "
          "see the other device\" question is answered by the networks section.");
    m.def("enable_crash_log", &enable_crash_log, py::arg("path"),
          "Write a crash report to `path` if the process dies on a fatal signal.\n\n"
          "Off unless called. Useful inside a host that has no terminal - a Slicer "
          "module or a Unity player - where the state at the moment of the crash is "
          "otherwise lost. Any handler already installed is chained to, so a host's "
          "own crash reporting keeps working.");
    m.def("disable_crash_log", &disable_crash_log);
    m.def("set_compression_level", &set_compression_level, py::arg("level"));
}
