// UniNet — correctness tests: CBOR round-trip, compression round-trip, envelope
// frame/unframe, and loopback pub/sub (echo suppression, dst targeting, wildcard).
#include "uninet/cbor.h"
#include "uninet/codec.h"
#include "uninet/loopback.h"
#include "uninet/node.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace uninet;

static int failures = 0;
#define CHECK(cond) do { if(!(cond)){ std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while(0)

static void test_cbor_roundtrip() {
    Cbor root = Cbor::map()
        .set("code", Cbor::text("update"))
        .set("update_type", Cbor::text("object"))
        .set("count", Cbor::uint(7))
        .set("active", Cbor::boolean(true))
        .set("temp", Cbor::f32(37.5f))
        .set("score", Cbor::f64(0.987654321))
        .set("neg", Cbor::integer(-42))
        .set("points", Cbor::f32_array(std::vector<float>{1, 2, 3, 4}.data(), 4))
        .set("blob", Cbor::bytes({0xDE, 0xAD, 0xBE, 0xEF}));

    Bytes enc = encode(root);
    bool ok = false;
    Cbor back = decode(enc.data(), enc.size(), &ok);
    CHECK(ok);
    CHECK(back.is_map());
    CHECK(back["code"].as_text() == "update");
    CHECK(back["count"].as_uint() == 7);
    CHECK(back["active"].as_bool() == true);
    CHECK(back["temp"].as_f32() == 37.5f);
    CHECK(std::abs(back["score"].as_f64() - 0.987654321) < 1e-9);
    CHECK(back["neg"].as_int() == -42);
    CHECK(back["points"].size() == 4);
    CHECK(back["points"].f32_items()[2] == 3.0f);
    CHECK(back["blob"].as_bytes() == Bytes({0xDE, 0xAD, 0xBE, 0xEF}));

    // Malformed input must fail gracefully.
    bool ok2 = true;
    Cbor bad = decode(reinterpret_cast<const uint8_t*>("\xFF\xFF"), 2, &ok2);
    CHECK(!ok2);
    CHECK(bad.is_null());
}

static void test_compression_roundtrip() {
    Bytes raw(4096);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = uint8_t((i * 31 + 7) & 0xFF);

    for (Compression m : {Compression::None, Compression::Zlib, Compression::Lz4}) {
#ifdef UNINET_HAS_LZ4
        // all three available
#else
        if (m == Compression::Lz4) continue;  // LZ4 not compiled in
#endif
        Bytes c = compress(raw, m);
        Bytes d = decompress(c, m);
        CHECK(d == raw);
    }
}

static void test_envelope_frame() {
    Envelope e;
    e.compression = Compression::Zlib;
    e.src_uuid = "server_xyz";
    e.dst_uuid = "";
    e.subject = "domain.D1";
    e.data = Cbor::map().set("hello", Cbor::text("world"));
    Bytes wire = frame(e);
    auto back = unframe(wire);
    CHECK(back.has_value());
    CHECK(back->src_uuid == "server_xyz");
    CHECK(back->subject == "domain.D1");
    CHECK(back->data["hello"].as_text() == "world");

    // Unknown compression byte -> reject.
    Bytes junk = {0x09, 0x01, 0x02};
    CHECK(!unframe(junk).has_value());
}

static void test_loopback_pubsub() {
    LoopbackTransport bus;
    bus.connect();
    Node a("alice", &bus);
    Node b("bob", &bus);
    a.connect();
    b.connect();

    int b_recv = 0, a_recv = 0;
    std::string last_text, last_subj;
    b.subscribe("domain.D1", [&](const Envelope& env) {
        ++b_recv;
        last_text = env.data["text"].as_text();
        last_subj = env.subject;
    });
    // alice subscribes too — must NOT receive her own echo.
    a.subscribe("domain.D1", [&](const Envelope&) { ++a_recv; });

    a.publish("domain.D1", Cbor::map().set("text", Cbor::text("hi bob")));
    CHECK(b_recv == 1);
    CHECK(last_text == "hi bob");
    CHECK(last_subj == "domain.D1");
    CHECK(a_recv == 0);   // echo suppressed

    // Wildcard subscription.
    int wild = 0;
    Node c("carol", &bus); c.connect();
    c.subscribe("domain.>", [&](const Envelope&) { ++wild; });
    a.publish("domain.D2", Cbor::map().set("text", Cbor::text("wild")));
    a.publish("other.X",   Cbor::map().set("text", Cbor::text("nope")));
    CHECK(wild == 1);

    // dst targeting: a unicast to bob is accepted by bob, ignored by carol.
    int carol_recv = 0;
    c.subscribe("direct", [&](const Envelope&) { ++carol_recv; });
    b.subscribe("direct", [&](const Envelope&) { ++b_recv; });
    a.publish("direct", Cbor::map().set("text", Cbor::text("only bob")), b.uuid());
    CHECK(carol_recv == 0);
    CHECK(b_recv == 2);
}

int main() {
    test_cbor_roundtrip();
    test_compression_roundtrip();
    test_envelope_frame();
    test_loopback_pubsub();
    if (failures == 0) { std::printf("UniNet round-trip: ALL TESTS PASSED\n"); return 0; }
    std::printf("UniNet round-trip: %d FAILURE(S)\n", failures);
    return 1;
}
