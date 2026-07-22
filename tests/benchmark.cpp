// UniNet benchmark — measures encode/decode, compression (none/zlib/lz4), and
// loopback pub/sub throughput so the README performance matrix can compare codecs.
//
// One build (LZ4 auto-detected at configure time); all compression methods are
// runtime-selectable. Run: ./benchmark [verts] [reps]  (defaults: 4096 verts, 200 reps).
#include "uninet/cbor.h"
#include "uninet/codec.h"
#include "uninet/loopback.h"
#include "uninet/node.h"
#include "uninet/profiler.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace uninet;

static std::string caps() {
    std::string s = "zlib";
#ifdef UNINET_HAS_LZ4
    s += "+lz4";
#else
    s += "(no-lz4)";
#endif
#ifdef UNINET_HAS_NATS
    s += "+nats";
#endif
    return s;
}

template <class F>
static double mean_ns(F&& f, int reps) {
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < reps; ++i) f();
    auto t1 = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
}

// A realistic ThermoNav "mesh update" payload: points (float32 x3, fast path) +
// flat triangle indices packed as little-endian uint32 bytes (the audit's "flat
// triangle-index list", packed — faster than per-element CBOR ints) + a 4x4
// transform. This is the kind of message the audit flagged as bandwidth-heavy.
static Cbor make_mesh_payload(int verts, Bytes& polys_out) {
    std::vector<float> pts(size_t(verts) * 3);
    for (size_t i = 0; i < pts.size(); ++i) pts[i] = float(i) * 0.123f;
    int tris = verts;  // one triangle per vertex (illustrative)
    polys_out.resize(size_t(tris) * 3 * 4);
    for (int i = 0; i < tris; ++i)
        for (int j = 0; j < 3; ++j) {
            uint32_t idx = uint32_t((i * 3 + j) % verts);
            std::memcpy(polys_out.data() + (size_t(i) * 3 + j) * 4, &idx, 4);
        }
    double xform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1.5, 2.5, 0, 1};
    return Cbor::map()
        .set("code", Cbor::text("update"))
        .set("update_type", Cbor::text("object"))
        .set("object", Cbor::map()
                .set("object_type", Cbor::text("surface"))
                .set("polydata", Cbor::map()
                        .set("points", Cbor::f32_array(std::move(pts)))
                        .set("polys", Cbor::bytes(polys_out)))
                .set("transform", Cbor::f64_array(xform, 16)));
}

int main(int argc, char** argv) {
    const int verts = (argc > 1) ? std::atoi(argv[1]) : 4096;
    const int reps  = (argc > 2) ? std::atoi(argv[2]) : 200;

    Bytes polys_scratch;
    Cbor payload = make_mesh_payload(verts, polys_scratch);
    Envelope env;
    env.subject = "domain.D1";
    env.src_uuid = "bench";
    env.data = payload;

    // Warm up.
    Bytes enc = encode(to_cbor(env));
    decode(enc.data(), enc.size());

    double enc_ns = mean_ns([&] { (void)encode(to_cbor(env)); }, reps);
    double dec_ns = mean_ns([&] { (void)decode(enc.data(), enc.size()); }, reps);
    const double payload_mb = double(enc.size()) / (1024.0 * 1024.0);

    std::printf("CAPS %s\n", caps().c_str());
    std::printf("payload: %d verts, %.2f KiB uncompressed CBOR\n", verts, enc.size() / 1024.0);
    std::printf("op,reps,mean_us,throughput\n");
    std::printf("encode,%d,%.3f,%.1f MB/s\n", reps, enc_ns / 1000.0,
                payload_mb / (enc_ns / 1e9));
    std::printf("decode,%d,%.3f,%.1f MB/s\n", reps, dec_ns / 1000.0,
                payload_mb / (dec_ns / 1e9));

    // ── compression matrix ──
    Compression methods[] = {Compression::None, Compression::Zlib, Compression::Lz4};
    const char* names[] = {"none", "zlib", "lz4"};
    for (size_t i = 0; i < 3; ++i) {
        Compression m = methods[i];
#ifdef UNINET_HAS_LZ4
#else
        if (m == Compression::Lz4) { std::printf("%s,skipped (not built),,\n", names[i]); continue; }
#endif
        double c_ns = mean_ns([&] { (void)compress(enc, m); }, reps);
        Bytes comp = compress(enc, m);
        double d_ns = mean_ns([&] { (void)decompress(comp, m); }, reps);
        double ratio = comp.empty() ? 0.0 : double(enc.size()) / double(comp.size());
        std::printf("compress_%s,%d,%.3f,%.1f MB/s (ratio %.2fx, wire %.1f KiB)\n",
                    names[i], reps, c_ns / 1000.0,
                    payload_mb / (c_ns / 1e9), ratio, comp.size() / 1024.0);
        std::printf("decompress_%s,%d,%.3f,%.1f MB/s\n",
                    names[i], reps, d_ns / 1000.0, payload_mb / (d_ns / 1e9));
    }

    // ── end-to-end loopback pub/sub (one publisher, one subscriber) ──
    LoopbackTransport bus;
    bus.connect();
    Node pub("pub", &bus); pub.connect();
    Node sub("sub", &bus); sub.connect();
    long got = 0;
    sub.subscribe("domain.D1", [&](const Envelope&) { ++got; });

    const int msgs = 5000;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < msgs; ++i)
        pub.publish("domain.D1", payload);
    auto t1 = std::chrono::high_resolution_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    std::printf("loopback_pubsub,%d,%.3f us/msg,%.0f msgs/s %.1f MB/s\n",
                msgs, sec * 1e6 / msgs, msgs / sec,
                (double(msgs) * enc.size()) / (1024.0 * 1024.0) / sec);

    // Profile one LZ4-framed publish for the codec/compress breakdown.
    profiler::enable(true);
    profiler::reset();
    env.compression = Compression::Lz4;
    (void)frame(env);
    std::fprintf(stderr, "%s", profiler::report().c_str());
    return got == msgs ? 0 : 1;
}
