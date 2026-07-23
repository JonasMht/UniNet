// UniNet — pybind11 bindings. Python drives the same compiled C++ core: build
// Cbor payloads, publish/subscribe over a LoopbackTransport Node, and profile.
// No duplicate logic vs the C++ server / the C# client.
#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include "uninet/cbor.h"
#include "uninet/codec.h"
#include "uninet/loopback.h"
#include "uninet/node.h"
#include "uninet/profiler.h"

#ifdef UNINET_HAS_NATS
#include "uninet/nats_transport.h"
#endif

#include <memory>
#include <string>

namespace py = pybind11;
using namespace uninet;

PYBIND11_MODULE(_uninet, m) {
    m.doc() = "UniNet compiled core: CBOR codec, compression, transport, pub/sub Node.";

    m.attr("PROTOCOL_VERSION") = int(CURRENT_PROTOCOL_VERSION);
#ifdef UNINET_HAS_LZ4
    m.attr("HAS_LZ4") = true;
#else
    m.attr("HAS_LZ4") = false;
#endif
#ifdef UNINET_HAS_NATS
    m.attr("HAS_NATS") = true;
#else
    m.attr("HAS_NATS") = false;
#endif

    py::enum_<Compression>(m, "Compression")
        .value("None", Compression::None)
        .value("Zlib", Compression::Zlib)
        .value("Lz4",  Compression::Lz4);

    // ── Cbor ──
    py::class_<Cbor>(m, "Cbor")
        .def(py::init<>())
        .def_static("map",   []() { return Cbor::map(); })
        .def_static("array", []() { return Cbor::array(); })
        .def_static("text",  [](const std::string& s) { return Cbor::text(s); })
        .def_static("bytes", [](const py::bytes& b) {
            std::string s = b; return Cbor::bytes(Bytes(s.begin(), s.end()));
        })
        .def_static("uint", [](uint64_t v) { return Cbor::uint(v); })
        .def_static("f32",  [](float v)    { return Cbor::f32(v); })
        .def_static("f64",  [](double v)   { return Cbor::f64(v); })
        .def_static("f32_array", [](const std::vector<float>& v) {
            return Cbor::f32_array(v.data(), v.size());
        })
        .def_static("f64_array", [](const std::vector<double>& v) {
            return Cbor::f64_array(v.data(), v.size());
        })
        .def("set", &Cbor::set, py::arg("key"), py::arg("value"),
             py::return_value_policy::reference_internal)
        .def("push_back", &Cbor::push_back, py::arg("value"),
             py::return_value_policy::reference_internal)
        .def("has", &Cbor::has, py::arg("key"))
        .def("__len__", &Cbor::size)
        .def("__getitem__", [](const Cbor& c, size_t i) -> const Cbor& { return c[i]; },
             py::return_value_policy::reference_internal)
        .def("__getitem__", [](const Cbor& c, const std::string& k) -> const Cbor& { return c[k]; },
             py::return_value_policy::reference_internal)
        .def_property_readonly("kind", [](const Cbor& c) { return int(c.kind()); })
        .def("as_text", &Cbor::as_text)
        .def("as_bytes", [](const Cbor& c) {
            const Bytes& b = c.as_bytes(); return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
        })
        .def("as_int",  &Cbor::as_int)
        .def("as_float", &Cbor::as_f64)
        .def("f32_items", [](const Cbor& c) { return c.f32_items(); })  // contiguous float array
        .def("f64_items", [](const Cbor& c) { return c.f64_items(); })
        .def("map_items", [](const Cbor& c) { return c.map_items(); })  // (key,value) pairs for dict decode
        .def("size", &Cbor::size);

    m.def("encode", [](const Cbor& c) {
        Bytes b = encode(c);
        return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
    });
    m.def("decode", [](const py::bytes& b) {
        std::string s = b; bool ok = false;
        return decode(reinterpret_cast<const uint8_t*>(s.data()), s.size(), &ok);
    });

    m.def("set_compression_level", &set_compression_level, py::arg("level"));
    m.def("compress", [](const py::bytes& raw, Compression mtd) {
        std::string s = raw; Bytes b = compress(Bytes(s.begin(), s.end()), mtd);
        return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
    });
    m.def("decompress", [](const py::bytes& comp, Compression mtd) {
        std::string s = comp; Bytes b = decompress(Bytes(s.begin(), s.end()), mtd);
        return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
    });

    // ── Envelope ──
    py::class_<Envelope>(m, "Envelope")
        .def_readwrite("protocol_version", &Envelope::protocol_version)
        .def_readwrite("compression", &Envelope::compression)
        .def_readwrite("src_uuid", &Envelope::src_uuid)
        .def_readwrite("dst_uuid", &Envelope::dst_uuid)
        .def_readwrite("subject", &Envelope::subject)
        .def_readwrite("data", &Envelope::data);

    // ── LoopbackTransport ──
    py::class_<LoopbackTransport, std::unique_ptr<LoopbackTransport>>(m, "LoopbackTransport")
        .def(py::init<>())
        .def("connect", &LoopbackTransport::connect)
        .def("disconnect", &LoopbackTransport::disconnect)
        .def("connected", &LoopbackTransport::connected)
        .def("delivered", &LoopbackTransport::delivered);

#ifdef UNINET_HAS_NATS
    // ── NatsTransport (the production brokered backend) ──
    py::class_<NatsTransport, std::unique_ptr<NatsTransport>>(m, "NatsTransport")
        .def(py::init([](const std::string& url) { return new NatsTransport(url); }),
             py::arg("url") = "nats://127.0.0.1:4222")
        .def("connect", &NatsTransport::connect)
        .def("disconnect", &NatsTransport::disconnect)
        .def("connected", &NatsTransport::connected);
#endif

    // ── Node (accepts a LoopbackTransport; NATS-backed variant staged) ──
    py::class_<Node, std::unique_ptr<Node>>(m, "Node")
        .def(py::init([](const std::string& name, LoopbackTransport* t, Compression c) {
            return new Node(name, t, c);
        }), py::arg("name"), py::arg("transport"), py::arg("compression") = DEFAULT_COMPRESSION)
#ifdef UNINET_HAS_NATS
        .def(py::init([](const std::string& name, NatsTransport* t, Compression c) {
            return new Node(name, t, c);
        }), py::arg("name"), py::arg("transport"), py::arg("compression") = DEFAULT_COMPRESSION)
#endif
        .def_property_readonly("uuid", &Node::uuid)
        .def("connect", &Node::connect)
        .def("connected", &Node::connected)
        .def("publish", &Node::publish, py::arg("subject"), py::arg("data"),
             py::arg("dst_uuid") = "")
        .def("subscribe", [](Node& n, const std::string& subject, py::function cb) {
            n.subscribe(subject, [cb](const Envelope& env) {
                py::gil_scoped_acquire gil;
                cb(env);
            });
        });

    // ── profiler ──
    py::module_ pf = m.def_submodule("profiler");
    pf.doc() = "Opt-in performance analytics. enable() -> run workload -> report().";
    pf.def("enable", [](bool on) { profiler::enable(on); }, py::arg("on") = true);
    pf.def("enabled", [] { return profiler::enabled(); });
    pf.def("reset", [] { return profiler::reset(); });
    pf.def("report", [] { return profiler::report(); });
}
