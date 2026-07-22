// UniNet benchmark — complete pipeline diagnostic. Mirrors UniVox's approach
// (operation-level ScopedOp instrumentation + an opt-in profiler report) but
// exercises the *whole* path (encode → compress → frame → deliver → unframe →
// decode) so the report pinpoints the dominant cost — the lever for the next
// "massive improvement".
//
// It also logs every run to a CSV (append) and dumps the latest profiler report
// to a file, so results accumulate across runs for before/after comparison.
//
//   ./build/benchmark [reps=300] [profile_msgs=2000]
#include "uninet/cbor.h"
#include "uninet/codec.h"
#include "uninet/loopback.h"
#include "uninet/node.h"
#include "uninet/profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
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

// CSV row logger — appends to `path`, writing the header on first creation.
static void log_row(const std::string& path, const std::string& run,
                    int verts, double payload_kib, const std::string& op,
                    double mean_us, double mbps, double extra) {
    std::ifstream test(path);
    bool empty = !test.good();
    std::ofstream out(path, std::ios::app);
    if (empty) out << "run,verts,payload_kib,op,mean_us,mbps,extra\n";
    out << run << ',' << verts << ',' << payload_kib << ',' << op << ','
        << mean_us << ',' << mbps << ',' << extra << '\n';
}

// A realistic ThermoNav "mesh update": points (float32 x3) + flat triangle
// indices packed as little-endian uint32 bytes + a 4x4 transform.
static Cbor make_mesh_payload(int verts, Bytes& polys_out) {
    std::vector<float> pts(size_t(verts) * 3);
    for (size_t i = 0; i < pts.size(); ++i) pts[i] = float(i) * 0.123f;
    int tris = verts;
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
    const int reps = (argc > 1) ? std::atoi(argv[1]) : 300;
    const int profile_msgs = (argc > 2) ? std::atoi(argv[2]) : 2000;
    const std::string log_path = (argc > 3) ? argv[3] : "uninet_bench_log.csv";

    const std::string run = caps();
    std::printf("# UniNet benchmark  caps=%s  reps=%d\n", run.c_str(), reps);
    std::printf("# logging to: %s   (append; header on first write)\n", log_path.c_str());
    std::printf("verts,payload_kib,op,mean_us,mbps,extra\n");

    const int sizes[] = {512, 4096, 16384};
    Compression pm = Compression::Lz4;
#ifdef UNINET_HAS_LZ4
#else
    pm = Compression::Zlib;
#endif

    for (int verts : sizes) {
        Bytes polys;
        Cbor payload = make_mesh_payload(verts, polys);
        Envelope env; env.subject = "domain.D1"; env.src_uuid = "bench"; env.data = payload;
        env.compression = pm;

        Bytes enc = encode(to_cbor(env));                 // warm up
        decode(enc.data(), enc.size());
        const double kib = double(enc.size()) / 1024.0;
        const double mb = double(enc.size()) / (1024.0 * 1024.0);

        auto out = [&](const char* op, double ns, double extra = 0.0) {
            double us = ns / 1000.0, mbps = mb / (ns / 1e9);
            std::printf("%d,%.1f,%s,%.3f,%.1f,%.3f\n", verts, kib, op, us, mbps, extra);
            log_row(log_path, run, verts, kib, op, us, mbps, extra);
        };

        out("encode",    mean_ns([&] { (void)encode(to_cbor(env)); }, reps));
        out("decode",    mean_ns([&] { (void)decode(enc.data(), enc.size()); }, reps));
        out("frame",     mean_ns([&] { (void)frame(env); }, reps));

        Bytes wire = frame(env);
        out("unframe",   mean_ns([&] { (void)unframe(wire); }, reps));

        // compression tiers
        Compression methods[] = {Compression::None, Compression::Zlib, Compression::Lz4};
        const char* mname[] = {"none", "zlib", "lz4"};
        for (size_t i = 0; i < 3; ++i) {
            Compression m = methods[i];
#ifndef UNINET_HAS_LZ4
            if (m == Compression::Lz4) continue;
#endif
            double c_ns = mean_ns([&] { (void)compress(enc, m); }, reps);
            Bytes comp = compress(enc, m);
            double ratio = comp.empty() ? 0.0 : double(enc.size()) / double(comp.size());
            char op[32]; std::snprintf(op, sizeof(op), "compress_%s", mname[i]);
            out(op, c_ns, ratio);
            double d_ns = mean_ns([&] { (void)decompress(comp, m); }, reps);
            std::snprintf(op, sizeof(op), "decompress_%s", mname[i]);
            out(op, d_ns, ratio);
        }

        // end-to-end loopback pub/sub (one publisher, one subscriber)
        LoopbackTransport bus; bus.connect();
        Node pub("pub", &bus); pub.connect();
        Node sub("sub", &bus); sub.connect();
        long got = 0;
        sub.subscribe("domain.D1", [&](const Envelope&) { ++got; });
        const int msgs = 4000;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < msgs; ++i) pub.publish("domain.D1", payload);
        auto t1 = std::chrono::high_resolution_clock::now();
        double sec = std::chrono::duration<double>(t1 - t0).count();
        out("loopback_pubsub", sec / msgs * 1e9, double(msgs) / sec);  // ns/msg; extra = msgs/s
        if (got != msgs) std::fprintf(stderr, "WARN: %ld/%d messages delivered\n", got, msgs);
    }

    // ── PROFILE: exercise the whole pipeline, then read the breakdown ──
    Bytes polys;
    Cbor payload = make_mesh_payload(4096, polys);
    profiler::enable(true);
    profiler::reset();
    {
        LoopbackTransport bus; bus.connect();
        Node pub("pub", &bus, pm); pub.connect();
        Node sub("sub", &bus); sub.connect();
        long got = 0;
        sub.subscribe("domain.D1", [&](const Envelope&) { ++got; });
        for (int i = 0; i < profile_msgs; ++i) pub.publish("domain.D1", payload);
        std::printf("\n# profiled %d %s-framed pub/sub messages @ 4096 verts (got %ld)\n",
                    profile_msgs, pm == Compression::Lz4 ? "lz4" : "zlib", got);
    }
    std::string rep = profiler::report();
    std::printf("%s", rep.c_str());
    { std::ofstream f("uninet_profile.txt"); f << "# UniNet profiler — " << caps() << "\n" << rep; }
    std::printf("# saved profiler report -> uninet_profile.txt\n");

    // Dominant-op callout (the next thing to optimize).
    auto snap = profiler::snapshot();
    std::string worst; double worst_t = 0, grand = 0;
    for (auto& [op, s] : snap.ops) { grand += s.total_seconds; if (s.total_seconds > worst_t) { worst_t = s.total_seconds; worst = op; } }
    if (grand > 0)
        std::printf("# DOMINANT op: '%s' = %.1f%% of profiled time  -> the next lever to pull\n",
                    worst.c_str(), 100.0 * worst_t / grand);

    profiler::enable(false);
    return 0;
}
