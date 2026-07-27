// UniNet — network tests. Two real nodes, a real UDP beacon, real TCP between
// them. This is the test the old suite did not have: everything before it was
// single-threaded loopback, which is why three data races and a use-after-free
// survived to be found by an audit rather than by CI.
//
// It is deliberately tolerant about timing (discovery is a network event, not a
// function call) and strict about outcomes.
#include "uninet/blob.h"
#include "uninet/json.h"
#include "uninet/session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
  #include <unistd.h>      // getpid, for a realm nobody else on this box shares
#endif

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

// Poll until `pred` holds or the deadline passes. Returns whether it held.
// Discovery is asynchronous, so asserting immediately after join() would test
// the scheduler rather than the code.
template <typename F>
bool wait_until(F pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

// Every test uses its own realm so a developer running the demo on the same
// machine — or two CI jobs on one build box — cannot perturb the results.
std::string unique_realm(const char* tag) {
    static std::atomic<unsigned> n{0};
    return std::string("uninet-test-") + tag + "-" +
           std::to_string(
#ifdef _WIN32
               0
#else
               ::getpid()
#endif
           ) + "-" + std::to_string(n.fetch_add(1));
}

// ── two nodes discover each other, with nothing configured ────────────────
void test_discovery() {
    std::printf("discovery\n");
    const std::string realm = unique_realm("disc");

    uninet::SessionConfig ca; ca.realm = realm; ca.role = "server";
    uninet::SessionConfig cb; cb.realm = realm; cb.role = "headset";

    auto a = uninet::Session::join("Node A", ca);
    auto b = uninet::Session::join("Node B", cb);

    check(a->connected(), "node A is on the network");
    check(b->connected(), "node B is on the network");

    check(wait_until([&] { return a->peers().size() == 1; }, std::chrono::seconds(20)),
          "A discovered B without being told any address");
    check(wait_until([&] { return b->peers().size() == 1; }, std::chrono::seconds(20)),
          "B discovered A without being told any address");

    const auto peers = a->peers();
    if (!peers.empty()) {
        check(peers[0].name == "Node B", "A sees B's chosen name");
        check(peers[0].role() == "headset", "role arrives with the discovery beacon");
        check(!peers[0].address.empty(), "A observed B's address");
        check(peers[0].uuid == b->uuid(), "the discovered uuid is the one B publishes under");
    } else {
        check(false, "A sees B's chosen name");
        check(false, "role arrives with the discovery beacon");
        check(false, "A observed B's address");
        check(false, "the discovered uuid is the one B publishes under");
    }
}

// ── a peer leaving is noticed ─────────────────────────────────────────────
void test_departure() {
    std::printf("departure\n");
    const std::string realm = unique_realm("exit");

    uninet::SessionConfig cfg; cfg.realm = realm;
    auto a = uninet::Session::join("Stayer", cfg);
    auto b = uninet::Session::join("Leaver", cfg);

    check(wait_until([&] { return a->peers().size() == 1; }, std::chrono::seconds(20)),
          "peer appeared");

    std::atomic<bool> lost{false};
    a->on_peer_lost([&](const uninet::Peer&) { lost.store(true); });

    b.reset();   // clean shutdown sends EXIT — no waiting out a timeout
    check(wait_until([&] { return lost.load(); }, std::chrono::seconds(20)),
          "departure reported promptly on clean shutdown");
    check(wait_until([&] { return a->peers().empty(); }, std::chrono::seconds(5)),
          "peer removed from the list");
}

// ── messages actually flow, broadcast and addressed ───────────────────────
void test_messaging() {
    std::printf("messaging\n");
    const std::string realm = unique_realm("msg");

    uninet::SessionConfig cfg; cfg.realm = realm;
    auto a = uninet::Session::join("Sender", cfg);
    auto b = uninet::Session::join("Receiver", cfg);
    auto c = uninet::Session::join("Bystander", cfg);

    check(wait_until([&] { return a->peers().size() == 2; }, std::chrono::seconds(20)),
          "all three nodes see each other");

    std::mutex mu;
    std::vector<std::string> b_got, c_got;
    b->subscribe("t.>", [&](const uninet::Envelope& e) {
        std::lock_guard<std::mutex> lk(mu);
        b_got.push_back(uninet::to_json(e.data));
    });
    c->subscribe("t.>", [&](const uninet::Envelope& e) {
        std::lock_guard<std::mutex> lk(mu);
        c_got.push_back(uninet::to_json(e.data));
    });

    // Broadcast: everyone but the sender receives it.
    a->publish_json("t.broadcast", "{\"n\":1}");
    check(wait_until([&] { std::lock_guard<std::mutex> lk(mu); return b_got.size() == 1; },
                     std::chrono::seconds(10)), "broadcast reached B");
    check(wait_until([&] { std::lock_guard<std::mutex> lk(mu); return c_got.size() == 1; },
                     std::chrono::seconds(10)), "broadcast reached C");

    // Addressed: only the named peer receives it. Over ZRE this is a real
    // unicast, so C never sees the bytes at all.
    a->publish_json("t.private", "{\"n\":2}", b->uuid());
    check(wait_until([&] { std::lock_guard<std::mutex> lk(mu); return b_got.size() == 2; },
                     std::chrono::seconds(10)), "addressed message reached its target");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    {
        std::lock_guard<std::mutex> lk(mu);
        check(c_got.size() == 1, "addressed message did NOT reach the bystander");
        check(!b_got.empty() && b_got.back() == "{\"n\":2}",
              "payload survived the JSON -> CBOR -> JSON round trip");
    }

    // Echo suppression is free on ZRE: a sender never hears its own SHOUT.
    std::atomic<int> own{0};
    a->subscribe("t.>", [&](const uninet::Envelope&) { own.fetch_add(1); });
    a->publish_json("t.broadcast", "{\"n\":3}");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    check(own.load() == 0, "a node does not receive its own broadcast");
}

// ── realms keep two setups apart on one network ───────────────────────────
void test_realm_isolation() {
    std::printf("realm isolation\n");
    uninet::SessionConfig clinical; clinical.realm = unique_realm("clinical");
    uninet::SessionConfig dev;      dev.realm      = unique_realm("dev");

    auto a = uninet::Session::join("Clinical", clinical);
    auto b = uninet::Session::join("Developer", dev);

    std::this_thread::sleep_for(std::chrono::seconds(3));
    check(a->peers().empty(), "a device in another realm is invisible");
    check(b->peers().empty(), "…in both directions");
}

// ── many threads publishing at once ───────────────────────────────────────
// The old suite never started a thread, which is how the races in Node and
// LoopbackTransport went unnoticed. Run this under -fsanitize=thread.
void test_concurrent_publish() {
    std::printf("concurrent publish\n");
    const std::string realm = unique_realm("conc");

    uninet::SessionConfig cfg; cfg.realm = realm;
    auto a = uninet::Session::join("Publisher", cfg);
    auto b = uninet::Session::join("Collector", cfg);

    check(wait_until([&] { return a->peers().size() == 1; }, std::chrono::seconds(20)),
          "publisher sees the collector");

    std::atomic<int> received{0};
    b->subscribe("load.>", [&](const uninet::Envelope&) { received.fetch_add(1); });

    constexpr int kThreads = 4, kEach = 50;
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i)
                a->publish_json("load.x", "{\"t\":" + std::to_string(t) +
                                          ",\"i\":" + std::to_string(i) + "}");
        });
    }
    for (auto& w : workers) w.join();

    // Evaluate the wait first: building the message inline would read `received`
    // in unspecified order relative to the wait and report a stale count.
    const bool all_arrived = wait_until([&] { return received.load() == kThreads * kEach; },
                                        std::chrono::seconds(20));
    check(all_arrived, "every message from every thread arrived exactly once (" +
                           std::to_string(received.load()) + "/" +
                           std::to_string(kThreads * kEach) + ")");
}

// ── subscribing from several threads must not duplicate delivery ──────────
void test_concurrent_subscribe() {
    std::printf("concurrent subscribe\n");
    const std::string realm = unique_realm("sub");

    uninet::SessionConfig cfg; cfg.realm = realm;
    auto a = uninet::Session::join("Talker", cfg);
    auto b = uninet::Session::join("Listener", cfg);

    check(wait_until([&] { return a->peers().size() == 1; }, std::chrono::seconds(20)),
          "nodes paired");

    std::atomic<int> hits{0};
    std::vector<std::thread> subs;
    for (int t = 0; t < 4; ++t)
        subs.emplace_back([&] {
            b->subscribe("dup.one", [&](const uninet::Envelope&) { hits.fetch_add(1); });
        });
    for (auto& s : subs) s.join();

    a->publish_json("dup.one", "{}");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    // Four handlers were registered, so four calls is right. More than four
    // means the internal watch subscription was installed more than once.
    check(hits.load() == 4,
          "one message fired each handler exactly once (" +
              std::to_string(hits.load()) + "/4)");
}

// ── JSON is the same data model in every language ─────────────────────────
void test_json_bridge() {
    std::printf("json bridge\n");
    bool ok = false;

    uninet::Cbor v = uninet::from_json(
        R"({"code":"update","n":42,"pi":3.5,"on":true,"none":null,)"
        R"("pts":[1.5,2.5,3.5],"nested":{"a":[1,2]}})", &ok);
    check(ok, "parses a representative message");
    check(v["code"].as_text() == "update", "text survives");
    check(v["n"].as_uint() == 42, "integers stay integers");
    check(v["pi"].as_f64() == 3.5, "floats survive");
    check(v["on"].as_bool(), "booleans survive");
    check(v["none"].is_null(), "null survives");
    check(v["pts"].size() == 3, "arrays survive");

    // The wire is CBOR; JSON is only the face. Same bytes either way.
    const uninet::Bytes wire = uninet::encode(v);
    bool dok = false;
    uninet::Cbor back = uninet::decode(wire.data(), wire.size(), &dok);
    check(dok, "encodes to CBOR and back");
    check(uninet::to_json(back) == uninet::to_json(v),
          "JSON -> CBOR -> JSON is stable");

    uninet::from_json("{\"a\":1,}", &ok);
    check(!ok, "rejects trailing commas rather than guessing");
    uninet::from_json("{\"a\":1} junk", &ok);
    check(!ok, "rejects trailing garbage");
    uninet::from_json(std::string(200, '[') , &ok);
    check(!ok, "refuses runaway nesting");
}

// ── large payloads: files, volumes, meshes ────────────────────────────────
void test_blob_transfer() {
    std::printf("large payload transfer\n");
    const std::string realm = unique_realm("blob");

    uninet::SessionConfig cfg; cfg.realm = realm;
    auto tx = uninet::Session::join("Blob Sender", cfg);
    auto rx = uninet::Session::join("Blob Receiver", cfg);
    check(wait_until([&] { return tx->peers().size() == 1; }, std::chrono::seconds(20)),
          "nodes paired");

    uninet::Blob send_side(*tx, "files");
    uninet::Blob recv_side(*rx, "files");

    std::mutex mu;
    std::vector<uninet::BlobInfo> done;
    std::vector<uninet::Bytes>    payloads;
    std::atomic<size_t> progress_calls{0};
    std::atomic<size_t> last_progress{0};

    recv_side.on_received([&](const uninet::BlobInfo& info, const uninet::Bytes& data) {
        std::lock_guard<std::mutex> lk(mu);
        done.push_back(info);
        payloads.push_back(data);
    });
    recv_side.on_progress([&](const uninet::BlobInfo&, size_t n) {
        progress_calls.fetch_add(1);
        last_progress.store(n);
    });

    // A payload several chunks long, with a recognisable pattern so a
    // reassembly bug shows up as wrong bytes rather than merely a wrong length.
    constexpr size_t kSize = 5 * 1024 * 1024 + 12345;   // deliberately not chunk-aligned
    uninet::Bytes original(kSize);
    for (size_t i = 0; i < kSize; ++i) original[i] = uint8_t((i * 31 + 7) & 0xFF);

    uninet::Cbor meta = uninet::Cbor::map();
    meta.set("dtype", uninet::Cbor::text("uint8"));
    meta.set("shape", uninet::Cbor::uint(kSize));

    send_side.send("test-payload", original, meta);

    check(wait_until([&] { std::lock_guard<std::mutex> lk(mu); return done.size() == 1; },
                     std::chrono::seconds(30)),
          "a multi-chunk transfer completed");

    {
        std::lock_guard<std::mutex> lk(mu);
        if (!done.empty()) {
            check(done[0].name == "test-payload", "the name survived");
            check(done[0].size == kSize, "the declared size matched");
            check(done[0].src == tx->uuid(), "the sender's uuid was attached");
            check(done[0].meta["dtype"].as_text() == "uint8", "metadata survived");
            check(payloads[0].size() == kSize, "every byte arrived");
            check(payloads[0] == original, "the bytes are byte-for-byte identical");
        } else {
            check(false, "the name survived");
            check(false, "the declared size matched");
            check(false, "the sender's uuid was attached");
            check(false, "metadata survived");
            check(false, "every byte arrived");
            check(false, "the bytes are byte-for-byte identical");
        }
    }
    check(progress_calls.load() > 1, "progress was reported as chunks arrived (" +
                                         std::to_string(progress_calls.load()) + " times)");
    check(last_progress.load() == kSize, "progress reached the full size");
    check(recv_side.incoming_count() == 0, "no transfer was left buffered");
}

// A transfer addressed to one peer must not be reassembled by anyone else.
void test_blob_addressed() {
    std::printf("addressed large payload\n");
    const std::string realm = unique_realm("blobdst");

    uninet::SessionConfig cfg; cfg.realm = realm;
    auto tx = uninet::Session::join("Sender", cfg);
    auto rx = uninet::Session::join("Target", cfg);
    auto by = uninet::Session::join("Bystander", cfg);
    check(wait_until([&] { return tx->peers().size() == 2; }, std::chrono::seconds(20)),
          "three nodes paired");

    uninet::Blob send_side(*tx, "files");
    uninet::Blob target(*rx, "files");
    uninet::Blob bystander(*by, "files");

    std::atomic<int> target_got{0}, bystander_got{0};
    target.on_received([&](const uninet::BlobInfo&, const uninet::Bytes&) { target_got.fetch_add(1); });
    bystander.on_received([&](const uninet::BlobInfo&, const uninet::Bytes&) { bystander_got.fetch_add(1); });

    uninet::Bytes payload(600 * 1024, 0xAB);
    send_side.send("private", payload, uninet::Cbor::null(), rx->uuid());

    check(wait_until([&] { return target_got.load() == 1; }, std::chrono::seconds(20)),
          "the addressed peer received it");
    std::this_thread::sleep_for(std::chrono::seconds(1));
    check(bystander_got.load() == 0, "the bystander received nothing at all");
}

// Two transfers running at once must not interleave into each other's buffers.
void test_blob_concurrent() {
    std::printf("concurrent large payloads\n");
    const std::string realm = unique_realm("blobconc");

    uninet::SessionConfig cfg; cfg.realm = realm;
    auto a  = uninet::Session::join("Sender A", cfg);
    auto b  = uninet::Session::join("Sender B", cfg);
    auto rx = uninet::Session::join("Receiver", cfg);
    check(wait_until([&] { return rx->peers().size() == 2; }, std::chrono::seconds(20)),
          "three nodes paired");

    uninet::Blob sa(*a, "files"), sb(*b, "files"), recv(*rx, "files");

    std::mutex mu;
    std::map<std::string, uninet::Bytes> got;
    recv.on_received([&](const uninet::BlobInfo& info, const uninet::Bytes& data) {
        std::lock_guard<std::mutex> lk(mu);
        got[info.name] = data;
    });

    uninet::Bytes pa(2 * 1024 * 1024, 0x11);
    uninet::Bytes pb(2 * 1024 * 1024, 0x22);
    std::thread ta([&] { sa.send("from-a", pa); });
    std::thread tb([&] { sb.send("from-b", pb); });
    ta.join();
    tb.join();

    check(wait_until([&] { std::lock_guard<std::mutex> lk(mu); return got.size() == 2; },
                     std::chrono::seconds(30)),
          "both transfers completed");
    std::lock_guard<std::mutex> lk(mu);
    check(got.count("from-a") && got["from-a"] == pa, "sender A's bytes are intact");
    check(got.count("from-b") && got["from-b"] == pb, "sender B's bytes are intact");
}

// An empty payload is a legitimate transfer, and a common off-by-one.
void test_blob_empty() {
    std::printf("empty large payload\n");
    const std::string realm = unique_realm("blobempty");

    uninet::SessionConfig cfg; cfg.realm = realm;
    auto tx = uninet::Session::join("Sender", cfg);
    auto rx = uninet::Session::join("Receiver", cfg);
    check(wait_until([&] { return tx->peers().size() == 1; }, std::chrono::seconds(20)),
          "nodes paired");

    uninet::Blob send_side(*tx, "files"), recv_side(*rx, "files");
    std::atomic<bool> arrived{false};
    std::atomic<size_t> size{999};
    recv_side.on_received([&](const uninet::BlobInfo&, const uninet::Bytes& d) {
        size.store(d.size());
        arrived.store(true);
    });

    uninet::Bytes empty;
    send_side.send("nothing", empty);
    check(wait_until([&] { return arrived.load(); }, std::chrono::seconds(15)),
          "a zero-byte transfer still completes");
    check(size.load() == 0, "and arrives empty");
}

}  // namespace

int main() {
    std::printf("UniNet network tests — %s\n\n", uninet::zyre_version_string().c_str());

    test_json_bridge();
    test_discovery();
    test_departure();
    test_messaging();
    test_realm_isolation();
    test_concurrent_publish();
    test_concurrent_subscribe();
    test_blob_transfer();
    test_blob_addressed();
    test_blob_concurrent();
    test_blob_empty();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
