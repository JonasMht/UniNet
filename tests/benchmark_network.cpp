// UniNet: end-to-end network benchmark.
//
// tests/benchmark.cpp measures the codec in isolation (encode, compress, frame).
// This one measures what a user actually experiences: two real nodes on the real
// network, one publishing and the other receiving, counting messages that
// completed the whole trip: encode, compress, frame, TCP, unframe, decompress,
// decode, dispatch.
//
// It also measures the number that matters most for the "no configuration"
// claim: how long it takes two devices to find each other from a cold start.
//
//     ./uninet-benchmark            # default sizes
//     ./uninet-benchmark 500        # 500 messages per size
//
// Numbers are written to stdout as a table and appended to
// uninet_network_bench.csv so runs accumulate for before/after comparison.
#include "uninet/json.h"
#include "uninet/session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
  #include <unistd.h>
#endif

namespace {

using clock_t_ = std::chrono::steady_clock;

double seconds_since(clock_t_::time_point t0) {
    return std::chrono::duration<double>(clock_t_::now() - t0).count();
}

std::string unique_realm() {
    return "uninet-bench-" +
#ifdef _WIN32
           std::to_string(0);
#else
           std::to_string(::getpid());
#endif
}

// A payload shaped like the traffic UniNet actually carries: a mesh. `verts`
// vertices means 3*verts floats, which is where the codec's float fast path and
// the compression tiers earn their keep.
uninet::Cbor make_mesh(int verts) {
    std::vector<float> pts(size_t(verts) * 3);
    // Pseudo-random rather than a linear ramp. A ramp compresses far better than
    // real coordinates do, which is exactly how the old benchmark reported an
    // LZ4 ratio the field never saw.
    uint32_t s = 12345;
    for (auto& p : pts) {
        s = s * 1664525u + 1013904223u;
        p = float(double(s) / double(UINT32_MAX)) * 200.0f - 100.0f;
    }
    uninet::Cbor poly = uninet::Cbor::map();
    poly.set("points", uninet::Cbor::f32_array(std::move(pts)));
    uninet::Cbor data = uninet::Cbor::map();
    data.set("code", uninet::Cbor::text("update"));
    data.set("polydata", std::move(poly));
    return data;
}

struct Result {
    int    verts = 0;
    size_t payload_bytes = 0;
    double msgs_per_s = 0;
    double mb_per_s = 0;
    double median_latency_ms = 0;
    double p99_latency_ms = 0;
    int    delivered = 0;
    int    sent = 0;
};

// One publisher, `count` messages of `verts` vertices. The receiver's counter is
// passed in rather than the session itself: the subscription is registered once,
// outside the timed loop, so re-subscribing cannot skew a run.
Result run_throughput(uninet::Session& tx,
                      std::atomic<int>& received,
                      std::vector<double>& latencies_ms,
                      int verts, int count) {
    Result r;
    r.verts = verts;
    r.sent  = count;

    const uninet::Cbor payload = make_mesh(verts);
    r.payload_bytes = uninet::encode(payload).size();

    received.store(0);
    latencies_ms.clear();
    latencies_ms.reserve(size_t(count));

    // Warm up: the first message pays for buffer growth and the TCP window.
    for (int i = 0; i < 5; ++i) tx.publish("bench.warmup", payload);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    received.store(0);
    latencies_ms.clear();

    const auto t0 = clock_t_::now();
    for (int i = 0; i < count; ++i) tx.publish("bench.mesh", payload);
    const double send_s = seconds_since(t0);

    // Let the tail arrive. A slow receiver is a real result, not a reason to
    // stop counting, but do not wait forever.
    const auto deadline = clock_t_::now() + std::chrono::seconds(30);
    while (received.load() < count && clock_t_::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    const double total_s = seconds_since(t0);
    r.delivered  = received.load();
    r.msgs_per_s = r.delivered / total_s;
    r.mb_per_s   = double(r.delivered) * double(r.payload_bytes) / total_s / (1024.0 * 1024.0);
    (void)send_s;

    if (!latencies_ms.empty()) {
        std::vector<double> sorted = latencies_ms;
        std::sort(sorted.begin(), sorted.end());
        r.median_latency_ms = sorted[sorted.size() / 2];
        r.p99_latency_ms    = sorted[size_t(double(sorted.size()) * 0.99)];
    }
    return r;
}

// Cold-start discovery: how long from "process starts" to "the other device is
// in my list". This is the number the whole zero-configuration claim rests on.
double measure_discovery_seconds(const std::string& realm) {
    uninet::SessionConfig cfg;
    cfg.realm = realm + "-disc";

    auto a = uninet::Session::join("Bench A", cfg);
    const auto t0 = clock_t_::now();
    auto b = uninet::Session::join("Bench B", cfg);

    const auto deadline = clock_t_::now() + std::chrono::seconds(30);
    while (a->peers().empty() && clock_t_::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    return a->peers().empty() ? -1.0 : seconds_since(t0);
}

void append_csv(const std::string& path, const std::vector<Result>& rows,
                double discovery_s) {
    const bool fresh = !std::ifstream(path).good();
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    if (fresh)
        out << "verts,payload_bytes,msgs_per_s,mb_per_s,median_latency_ms,"
               "p99_latency_ms,delivered,sent,discovery_s\n";
    for (const auto& r : rows) {
        out << r.verts << ',' << r.payload_bytes << ',' << r.msgs_per_s << ','
            << r.mb_per_s << ',' << r.median_latency_ms << ',' << r.p99_latency_ms
            << ',' << r.delivered << ',' << r.sent << ',' << discovery_s << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int count = (argc > 1) ? std::atoi(argv[1]) : 300;
    const std::string csv = (argc > 2) ? argv[2] : "uninet_network_bench.csv";
    const std::string realm = unique_realm();

    std::printf("UniNet network benchmark: %s\n", uninet::zyre_version_string().c_str());
    std::printf("%d messages per size, loopback network (two processes' worth of\n"
                "work in one process: the bytes still cross the full stack).\n\n", count);

    // ── discovery ──
    std::printf("Measuring cold-start discovery...\n");
    const double discovery_s = measure_discovery_seconds(realm);
    if (discovery_s < 0)
        std::printf("  discovery FAILED (no peer found within 30 s)\n\n");
    else
        std::printf("  two devices found each other in %.3f s, with nothing configured\n\n",
                    discovery_s);

    // ── throughput ──
    uninet::SessionConfig cfg;
    cfg.realm = realm;
    auto tx = uninet::Session::join("Bench TX", cfg);
    auto rx = uninet::Session::join("Bench RX", cfg);

    // Wait for the pair to pair up before timing anything.
    for (int i = 0; i < 300 && tx->peers().empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (tx->peers().empty()) {
        std::fprintf(stderr, "benchmark: the two sessions never found each other: "
                             "is UDP 5670 blocked on this machine?\n");
        return 1;
    }

    std::atomic<int> received{0};
    std::vector<double> latencies_ms;
    rx->subscribe("bench.mesh", [&](const uninet::Envelope&) {
        received.fetch_add(1);
    });

    const int sizes[] = {512, 4096, 16384};
    std::vector<Result> rows;
    std::printf("%-9s %12s %12s %12s %10s\n",
                "VERTS", "PAYLOAD", "MSGS/S", "MB/S", "DELIVERED");
    for (int verts : sizes) {
        Result r = run_throughput(*tx, received, latencies_ms, verts, count);
        rows.push_back(r);
        std::printf("%-9d %9zu B %12.0f %12.1f %6d/%d\n",
                    r.verts, r.payload_bytes, r.msgs_per_s, r.mb_per_s,
                    r.delivered, r.sent);
        std::fflush(stdout);
    }

    append_csv(csv, rows, discovery_s);
    std::printf("\nAppended to %s\n", csv.c_str());

    // A delivery shortfall is the interesting failure: it means the receiver
    // could not keep up and ZeroMQ's high-water mark dropped messages.
    for (const auto& r : rows) {
        if (r.delivered < r.sent) {
            std::printf("\nNOTE: %d of %d messages did not arrive at %d verts: the\n"
                        "receiver could not keep up at this rate.\n",
                        r.sent - r.delivered, r.sent, r.verts);
        }
    }
    return 0;
}
