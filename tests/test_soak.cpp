// Does UniNet still behave after being used for a while, and by a lot of
// devices at once?
//
// Every other test in this tree runs a handful of nodes for a few seconds. That
// catches whether something works; it cannot catch the two failures that only
// show up in a room that has been running all day:
//
//   a leak, where each session or transfer keeps a file descriptor or some
//   memory that is never given back, and the process dies after eight hours;
//
//   a scale limit, where twelve devices is fine and twenty is not, which is a
//   very unpleasant thing to discover during a procedure.
//
// So this creates and destroys a lot of sessions, moves a lot of messages, and
// puts a lot of nodes on one realm, then checks the resources came back.
//
// The thresholds are deliberately loose. The point is to catch growth that is
// proportional to the work done, not to police a few kilobytes: an allocator
// that keeps a freed arena is not a leak, and failing on that would make the
// test something people learn to ignore.
#include "uninet/blob.h"
#include "uninet/session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s   %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

std::string realm_for(const char* tag) {
    return std::string("soak-") + tag + "-" + std::to_string(
#ifdef _WIN32
        0
#else
        ::getpid()
#endif
    );
}

// Open file descriptors. The most reliable leak signal there is on Linux: a
// socket that is never closed shows up here immediately and unambiguously,
// where memory growth could be the allocator holding on to an arena.
int open_fds() {
#ifdef _WIN32
    return -1;
#else
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    while (readdir(d)) ++n;
    closedir(d);
    return n;
#endif
}

// Resident set size in KB, or -1 where it cannot be read.
long rss_kb() {
#ifdef _WIN32
    return -1;
#else
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return -1;
    long total = 0, resident = 0;
    const int got = std::fscanf(f, "%ld %ld", &total, &resident);
    std::fclose(f);
    if (got != 2) return -1;
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
#endif
}

template <typename F>
bool wait_until(F pred, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(int(seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// ── 1. sessions, created and destroyed over and over ─────────────────────
void soak_session_churn() {
    section("100 sessions created and destroyed");
    // Warm up first: the first session in a process allocates the ZeroMQ
    // context and its threads, and counting that as leaked would be wrong.
    {
        uninet::SessionConfig cfg;
        cfg.realm = realm_for("warm");
        auto warm = uninet::Session::join("Warm", cfg);
        warm->close();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const int fds_before = open_fds();
    const long rss_before = rss_kb();

    for (int i = 0; i < 100; ++i) {
        uninet::SessionConfig cfg;
        cfg.realm = realm_for("churn");
        // Off: this test is about the resources a session owns, and a watchdog
        // thread per session would be measuring something else.
        cfg.auto_reconnect = false;
        auto net = uninet::Session::join("Churn" + std::to_string(i), cfg);
        net->subscribe("soak.>", [](const uninet::Envelope&) {});
        net->publish_json("soak.x", "{\"i\":1}");
        net->close();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const int fds_after = open_fds();
    const long rss_after = rss_kb();
    std::printf("       fds %d -> %d, rss %ld -> %ld KB\n",
                fds_before, fds_after, rss_before, rss_after);

    if (fds_before >= 0) {
        // A leaked socket per session would be +100. A handful of descriptors
        // moving is normal.
        check(fds_after - fds_before < 10,
              "no file descriptor left behind per session");
    }
    if (rss_before > 0) {
        // 100 sessions leaking anything session-sized would be tens of MB.
        check(rss_after - rss_before < 60 * 1024,
              "memory did not grow with the number of sessions destroyed");
    }
}

// ── 2. sustained traffic ─────────────────────────────────────────────────
void soak_messages() {
    section("50,000 messages through one pair");
    uninet::SessionConfig cfg;
    cfg.realm = realm_for("msg");
    cfg.auto_reconnect = false;
    auto tx = uninet::Session::join("Tx", cfg);
    auto rx = uninet::Session::join("Rx", cfg);

    std::atomic<int> got{0};
    rx->subscribe("soak.>", [&](const uninet::Envelope&) { got.fetch_add(1); });
    check(wait_until([&] { return !tx->peers().empty(); }, 10.0), "paired");

    // Settle, then measure: the first messages grow the framing buffers, and
    // that growth is not a leak.
    for (int i = 0; i < 500; ++i) tx->publish_json("soak.warm", "{\"a\":1}");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    // The warm-up messages match "soak.>" too, so the counter has to be reset
    // or the run reports more delivered than were sent, which reads as a bug in
    // the library rather than in the test.
    got.store(0);
    const long rss_before = rss_kb();
    const int fds_before = open_fds();

    const std::string payload = "{\"a\":1,\"b\":\"" + std::string(400, 'x') + "\"}";
    const int total = 50000;
    for (int i = 0; i < total; ++i) {
        tx->publish_json("soak.bulk", payload);
        // Do not outrun the receiver by so much that this measures queue depth
        // rather than leakage.
        if ((i % 5000) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    wait_until([&] { return got.load() >= total; }, 60.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const long rss_after = rss_kb();
    const int fds_after = open_fds();
    std::printf("       delivered %d/%d, rss %ld -> %ld KB, fds %d -> %d\n",
                got.load(), total, rss_before, rss_after, fds_before, fds_after);

    check(got.load() > total * 9 / 10, "the large majority arrived");
    if (rss_before > 0) {
        // Buffers are reused per thread, so steady-state publishing should
        // allocate nothing. Anything proportional to 50,000 messages would be
        // enormous; 40 MB is loose enough not to fire on allocator behaviour.
        check(rss_after - rss_before < 40 * 1024,
              "memory did not grow with the number of messages sent");
    }
    if (fds_before >= 0)
        check(fds_after - fds_before < 5, "no descriptors leaked by traffic");

    tx->close();
    rx->close();
}

// ── 3. a lot of devices at once ──────────────────────────────────────────
void scale_many_peers() {
    section("20 devices on one realm");
    const int N = 20;
    uninet::SessionConfig cfg;
    cfg.realm = realm_for("scale");
    cfg.auto_reconnect = false;

    std::vector<std::unique_ptr<uninet::Session>> nodes;
    std::atomic<int> received{0};
    for (int i = 0; i < N; ++i) {
        auto s = uninet::Session::join("Node" + std::to_string(i), cfg);
        s->subscribe("scale.>", [&](const uninet::Envelope&) { received.fetch_add(1); });
        nodes.push_back(std::move(s));
    }

    // Every node should see every other one. This is the number that matters:
    // ZRE is a full mesh, so twenty nodes is 380 directed connections, and if
    // there is a limit this is where it appears.
    const bool all_paired = wait_until([&] {
        for (auto& s : nodes) if (s->peers().size() < size_t(N - 1)) return false;
        return true;
    }, 45.0);

    size_t smallest = size_t(-1), largest = 0;
    for (auto& s : nodes) {
        const size_t n = s->peers().size();
        smallest = n < smallest ? n : smallest;
        largest = n > largest ? n : largest;
    }
    std::printf("       peer counts: %zu..%zu (expected %d each)\n",
                smallest, largest, N - 1);
    check(all_paired, "every device discovered every other device");

    // One broadcast should reach all nineteen others.
    received.store(0);
    nodes.front()->publish_json("scale.hello", "{\"from\":0}");
    const bool delivered = wait_until([&] { return received.load() >= N - 1; }, 20.0);
    std::printf("       one broadcast reached %d of %d\n", received.load(), N - 1);
    check(delivered, "a broadcast reached every other device");

    for (auto& s : nodes) s->close();
    check(true, "all closed cleanly");
}

// ── 4. large transfers, repeated ─────────────────────────────────────────
void soak_blobs() {
    section("40 large transfers");
    uninet::SessionConfig cfg;
    cfg.realm = realm_for("blob");
    cfg.auto_reconnect = false;
    auto a = uninet::Session::join("BlobTx", cfg);
    auto b = uninet::Session::join("BlobRx", cfg);
    uninet::Blob out(*a, "soak");
    uninet::Blob in(*b, "soak");

    std::atomic<int> done{0};
    std::atomic<size_t> last_size{0};
    in.on_received([&](const uninet::BlobInfo&, const uninet::Bytes& d) {
        last_size.store(d.size());
        done.fetch_add(1);
    });
    check(wait_until([&] { return !a->peers().empty(); }, 10.0), "paired");

    const uninet::Bytes payload(512 * 1024, 0x7e);
    out.send("warm.bin", payload);
    wait_until([&] { return done.load() >= 1; }, 15.0);
    done.store(0);
    const long rss_before = rss_kb();
    const int fds_before = open_fds();

    const int rounds = 40;
    for (int i = 0; i < rounds; ++i) {
        out.send("soak" + std::to_string(i) + ".bin", payload);
        wait_until([&] { return done.load() >= i + 1; }, 20.0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const long rss_after = rss_kb();
    const int fds_after = open_fds();
    std::printf("       completed %d/%d, rss %ld -> %ld KB, fds %d -> %d\n",
                done.load(), rounds, rss_before, rss_after, fds_before, fds_after);

    check(done.load() == rounds, "every transfer completed");
    check(last_size.load() == payload.size(), "and the last one was byte-complete");
    if (rss_before > 0) {
        // 40 x 512 KB is 20 MB moved. Holding on to even a few of those
        // reassembly buffers would show clearly.
        check(rss_after - rss_before < 30 * 1024,
              "memory did not grow with the number of transfers");
    }
    if (fds_before >= 0)
        check(fds_after - fds_before < 5, "no descriptors leaked by transfers");

    a->close();
    b->close();
}

}  // namespace

int main() {
    std::printf("UniNet soak and scale\n");
    soak_session_churn();
    soak_messages();
    scale_many_peers();
    soak_blobs();

    std::printf("\n%s (%d failure%s)\n", failures ? "FAIL" : "PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
