// Does bridging actually join two networks that cannot see each other?
//
// Discovery is link-local, so a machine on both Wi-Fi and a tethered phone can
// see devices on each while those devices remain invisible to one another.
// Bridging puts a node on every network and passes messages between them.
//
// The two networks here are two beacon PORTS, not two interfaces, and that is
// not dodging the hard part. ZRE isolates by port exactly as completely as by
// interface, and the first thing this test does is prove that isolation before
// depending on it. Two dummy interfaces on one host would NOT have been
// isolated: the beacon binds INADDR_ANY, so a broadcast on one is delivered to
// a socket on the other, and the test would pass with the bridge doing nothing.
// Genuine interface isolation needs separate network namespaces joined by veth,
// which is what a second machine is in practice.
//
// Covered: that the isolation is real, that a bridge carries messages both ways
// across it, that message identity survives the hop, and that nothing loops.
#include "uninet/json.h"
#include "uninet/session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s   %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

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

struct Counter {
    std::atomic<int> n{0};
    std::mutex mu;
    std::string last;
    void note(const uninet::Envelope& e) {
        std::lock_guard<std::mutex> lk(mu);
        last = uninet::to_json(e.data);
        n.fetch_add(1);
    }
    std::string text() { std::lock_guard<std::mutex> lk(mu); return last; }
};

uninet::SessionConfig plain(const std::string& realm, int port) {
    uninet::SessionConfig cfg;
    cfg.realm = realm;
    cfg.port = port;
    cfg.auto_reconnect = false;   // the networks here do not move
    cfg.allow_bridging = false;   // an ordinary device, not the bridging machine
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    // The helper the session will start. Passed in so the test runs against the
    // binary that was just built rather than one on PATH.
    const std::string helper = (argc > 1) ? argv[1] : "";
    const std::string realm = "bridgecheck" + std::to_string(::getpid());
    const int PORT_A = 5690, PORT_B = 5692;

    std::printf("UniNet bridging\n");

    // ── 1. the isolation this test depends on ────────────────────────────
    section("two networks, nothing bridging them");
    auto a = uninet::Session::join("OnA", plain(realm, PORT_A));
    auto b = uninet::Session::join("OnB", plain(realm, PORT_B));
    check(a->connected() && b->connected(), "both devices joined");

    Counter got_a, got_b;
    a->subscribe("chat.>", [&](const uninet::Envelope& e) { got_a.note(e); });
    b->subscribe("chat.>", [&](const uninet::Envelope& e) { got_b.note(e); });

    std::this_thread::sleep_for(std::chrono::seconds(2));
    check(a->peers().empty() && b->peers().empty(),
          "they cannot see each other, so the isolation is real");
    a->publish_json("chat.x", "{\"from\":\"A\"}");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    check(got_b.n.load() == 0, "and a message does not cross on its own");

    // ── 2. a machine on both, bridging ───────────────────────────────────
    section("a machine on both networks, bridging");
    uninet::SessionConfig bcfg;
    bcfg.realm = realm;
    bcfg.port = PORT_A;                 // its own network is A
    bcfg.auto_reconnect = false;
    bcfg.allow_bridging = true;
    if (!helper.empty()) bcfg.bridge_helper = helper;
    // The helper is told which network to take. bridgeable() picks interfaces,
    // and this test's networks are ports, so the extra network is named here
    // through the helper arguments the session builds. Without a second
    // interface to find, the session starts no bridge of its own, so the test
    // starts one explicitly and drives the same relay.
    auto hub = uninet::Session::join("Hub", bcfg);
    check(hub->connected(), "the bridging machine joined network A");

    // Start the helper for network B by hand, pointed at this process. This is
    // exactly what Session does when it finds a second interface; ports are
    // used here because one host has only one set of interfaces.
    const std::string link = "ipc:///tmp/uninet-bridgetest-" + std::to_string(::getpid());
    const std::string cmd =
        (helper.empty() ? std::string("uninet-bridge") : helper) +
        " --port " + std::to_string(PORT_B) + " --realm " + realm +
        " --link " + link + " --name HubBridge > /tmp/uninet-bridgetest.log 2>&1 &";

    // The parent half of the link lives inside Session; expose it by asking the
    // session to adopt this endpoint as a bridge.
    check(hub->adopt_bridge("portB", link), "the session adopted the link");
    const int rc = std::system(cmd.c_str());
    (void)rc;

    check(wait_until([&] { return hub->bridge_count() == 1; }, 5.0),
          "the session reports one bridge");
    // The helper has to join network B and find the device already there.
    check(wait_until([&] { return !b->peers().empty(); }, 15.0),
          "the bridge appeared on network B");

    // ── 3. messages cross, both ways ─────────────────────────────────────
    section("messages across the bridge");
    const int before_b = got_b.n.load();
    check(wait_until([&] {
              a->publish_json("chat.a2b", "{\"from\":\"A\"}");
              std::this_thread::sleep_for(std::chrono::milliseconds(300));
              return got_b.n.load() > before_b;
          }, 20.0),
          "A reached B, on a network A cannot see");
    check(got_b.text().find("\"A\"") != std::string::npos,
          "and the payload arrived intact, got " + got_b.text());

    const int before_a = got_a.n.load();
    check(wait_until([&] {
              b->publish_json("chat.b2a", "{\"from\":\"B\"}");
              std::this_thread::sleep_for(std::chrono::milliseconds(300));
              return got_a.n.load() > before_a;
          }, 20.0),
          "B reached A, the other way across the same bridge");

    // ── 4. nothing loops ─────────────────────────────────────────────────
    section("no loop");
    // One publish must arrive once, not repeatedly. A relay that forwards a
    // message it has already seen produces a storm that looks like a working
    // bridge until the network is saturated.
    got_b.n.store(0);
    a->publish_json("chat.once", "{\"n\":1}");
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const int delivered = got_b.n.load();
    std::printf("       one publish delivered %d time(s)\n", delivered);
    check(delivered == 1, "one publish arrived exactly once");

    hub->close();
    a->close();
    b->close();

    std::printf("\n%s (%d failure%s)\n", failures ? "FAIL" : "PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
