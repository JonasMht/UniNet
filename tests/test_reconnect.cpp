// Does UniNet survive the network changing under it?
//
// Discovery binds one interface when the beacon starts, and ZRE never rebinds.
// So Wi-Fi dropping, moving between access points, a cable pulled or a phone
// tethered used to leave a session bound to an interface that no longer exists:
// deaf and invisible, with nothing reported, until the application restarted.
//
// This changes the machine's networks while sessions are running and checks not
// only that they notice, but that everything they were doing still works
// afterwards: discovery, broadcast, addressed messages, large transfers, and
// shutdown.
//
// It has to create and destroy interfaces, which normally means root. It does
// not here: the runner puts it in a private network namespace with `unshare
// -rn`, where an unprivileged user is root and the interfaces are synthetic and
// invisible to the rest of the machine. See scripts/test-reconnect.sh. Run
// outside such a namespace it skips rather than failing, because there it would
// be testing the developer's real network.
#include "uninet/blob.h"
#include "uninet/session.h"
#include "uninet/zyre_transport.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s   %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

void section(const std::string& title) {
    std::printf("\n%s\n", title.c_str());
}

bool run(const std::string& cmd) { return std::system(cmd.c_str()) == 0; }

// Wait for a predicate rather than sleeping a fixed time: the supervisor polls,
// so a fixed sleep is either flaky or slow.
template <typename F>
bool wait_until(F pred, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(int(seconds * 1000));
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

void add_iface(const std::string& name, const std::string& cidr) {
    run("ip link add " + name + " type dummy 2>/dev/null");
    run("ip addr add " + cidr + " dev " + name + " 2>/dev/null");
    run("ip link set " + name + " up");
}

uninet::SessionConfig fast(const std::string& realm) {
    uninet::SessionConfig cfg;
    cfg.realm = realm;
    cfg.reconnect_poll_ms = 300;   // seconds, not a minute
    return cfg;
}

// ── 1. one session, the network moving under it ──────────────────────────
void scenario_interface_moves() {
    section("a session follows the network it should be on");
    uninet::ZyreConfig cfg;
    cfg.realm = "reconnect-move";
    cfg.reconnect_poll_ms = 300;
    uninet::ZyreTransport net("Mover", cfg);
    check(net.connect(), "joined");
    check(net.chosen_interface().name == "eth0",
          "picked eth0, got '" + net.chosen_interface().name + "'");

    // A supervisor that rebuilds on its own schedule would churn the network
    // forever and be worse than none. This is the check that catches it.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    check(net.reconnect_count() == 0, "no rebuild while the network is stable");

    add_iface("usb0", "192.168.60.1/24");     // a phone is tethered
    check(wait_until([&] { return net.reconnect_count() >= 1; }, 5.0),
          "noticed the better network and rebuilt");
    check(wait_until([&] { return net.chosen_interface().name == "usb0"; }, 5.0),
          "moved to usb0, now on '" + net.chosen_interface().name + "'");
    check(net.connected(), "still connected after the move");

    const uint64_t before = net.reconnect_count();
    run("ip link del usb0");                  // unplugged
    check(wait_until([&] { return net.reconnect_count() > before; }, 5.0),
          "noticed it disappear and rebuilt");
    check(wait_until([&] { return net.chosen_interface().name == "eth0"; }, 5.0),
          "fell back to eth0, now on '" + net.chosen_interface().name + "'");
    check(net.connected(), "still connected after falling back");
    net.disconnect();
}

// ── 2. two peers keep talking across a network change ────────────────────
// The one that matters. A session that reconnects but can no longer exchange
// messages has survived nothing.
void scenario_peers_keep_talking() {
    section("two peers keep talking across a network change");
    auto a = uninet::Session::join("Alice", fast("reconnect-talk"));
    auto b = uninet::Session::join("Bob", fast("reconnect-talk"));

    std::atomic<int> got_b{0};
    std::mutex last_mu;
    std::string last;
    b->subscribe("chat.>", [&](const uninet::Envelope& e) {
        std::lock_guard<std::mutex> lk(last_mu);
        last = e.subject;
        got_b.fetch_add(1);
    });

    check(wait_until([&] { return !a->peers().empty() && !b->peers().empty(); }, 8.0),
          "found each other");
    a->publish_json("chat.one", "{\"n\":1}");
    check(wait_until([&] { return got_b.load() >= 1; }, 3.0), "a message arrived before the change");

    // Move the machine. Both sessions are on this machine, so both rebuild.
    const uint64_t ra = a->transport().reconnect_count();
    add_iface("usb0", "192.168.60.1/24");
    check(wait_until([&] { return a->transport().reconnect_count() > ra; }, 6.0),
          "the network change was noticed");

    // Re-discovery is not instant: the old identities exited and new ones have
    // to beacon. This is the window an application would see as a blip.
    check(wait_until([&] { return !a->peers().empty() && !b->peers().empty(); }, 12.0),
          "found each other again on the new network");

    const int before = got_b.load();
    check(wait_until([&] {
              a->publish_json("chat.two", "{\"n\":2}");
              std::this_thread::sleep_for(std::chrono::milliseconds(200));
              return got_b.load() > before;
          }, 10.0),
          "broadcast still works after the change");
    {
        std::lock_guard<std::mutex> lk(last_mu);
        check(last == "chat.two", "and the right message arrived, got '" + last + "'");
    }

    // Addressed messages are the sharp case: they are filtered on the uuid,
    // which the rebuild REPLACED. If the Node kept the old one, every unicast
    // to this device would be dropped while broadcasts kept working.
    std::atomic<int> got_direct{0};
    b->subscribe("direct.>", [&](const uninet::Envelope&) { got_direct.fetch_add(1); });
    check(wait_until([&] {
              const auto peers = a->peers();
              if (peers.empty()) return false;
              a->publish_json("direct.x", "{\"hi\":1}", peers.front().uuid);
              std::this_thread::sleep_for(std::chrono::milliseconds(200));
              return got_direct.load() >= 1;
          }, 10.0),
          "addressed messages still reach the peer after its identity changed");

    check(a->uuid() == a->node().uuid(),
          "the session and its node agree on the identity after the rebuild");

    a->close();
    b->close();
    run("ip link del usb0");
}

// ── 3. a large transfer while the network changes ────────────────────────
void scenario_blob_survives() {
    section("a large transfer, with the network changing underneath");
    auto a = uninet::Session::join("Sender", fast("reconnect-blob"));
    auto b = uninet::Session::join("Receiver", fast("reconnect-blob"));
    uninet::Blob out(*a, "files");
    uninet::Blob in(*b, "files");

    std::atomic<int> received{0};
    std::atomic<size_t> bytes{0};
    in.on_received([&](const uninet::BlobInfo&, const uninet::Bytes& data) {
        bytes.store(data.size());
        received.fetch_add(1);
    });
    check(wait_until([&] { return !a->peers().empty(); }, 8.0), "paired");

    const uninet::Bytes payload(256 * 1024, 0x5a);
    check(!out.send("before.bin", payload).empty(), "a transfer started");
    check(wait_until([&] { return received.load() >= 1; }, 10.0), "it completed");
    check(bytes.load() == payload.size(), "every byte arrived");

    // A transfer interrupted by a rebuild should fail or be abandoned, never be
    // delivered truncated: a half file that reports success is the worst
    // outcome of the three.
    add_iface("usb0", "192.168.60.1/24");
    const int done_before = received.load();
    out.send("during.bin", payload);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const int done_after = received.load();
    check(done_after == done_before || bytes.load() == payload.size(),
          "a transfer across the change either did not arrive or arrived whole");

    check(wait_until([&] { return !a->peers().empty(); }, 12.0), "paired again");
    received.store(0); bytes.store(0);
    check(wait_until([&] {
              out.send("after.bin", payload);
              std::this_thread::sleep_for(std::chrono::milliseconds(500));
              return received.load() >= 1;
          }, 15.0),
          "transfers work again after the change");
    check(bytes.load() == payload.size(), "and are still byte-complete");

    a->close(); b->close();
    run("ip link del usb0");
}

// ── 4. abuse ─────────────────────────────────────────────────────────────
void scenario_flapping() {
    section("a flapping network");
    auto net = uninet::Session::join("Flapper", fast("reconnect-flap"));
    check(net->connected(), "joined");

    // Ten changes back to back. The point is not that every one is observed,
    // but that the session is alive and coherent at the end rather than
    // deadlocked, crashed, or stuck without a node.
    for (int i = 0; i < 10; ++i) {
        add_iface("usb0", "192.168.60.1/24");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        run("ip link del usb0");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    check(wait_until([&] { return net->connected(); }, 10.0), "still connected after flapping");
    check(wait_until([&] { return net->transport().chosen_interface().name == "eth0"; }, 8.0),
          "settled back on eth0, on '" + net->transport().chosen_interface().name + "'");
    check(net->publish_json("x.y", "{\"a\":1}") || !net->peers().empty() || true,
          "publish does not throw or hang afterwards");
    net->close();
    check(true, "closed cleanly after flapping");
}

void scenario_close_during_change() {
    section("closing while the network is changing");
    // Closing has to stop the watchdog before the Node is released: the
    // reconnect callback holds a raw pointer to it. Getting this wrong is a
    // use-after-free that only appears under exactly this timing.
    for (int i = 0; i < 5; ++i) {
        auto net = uninet::Session::join("Closer", fast("reconnect-close"));
        add_iface("usb0", "192.168.60.1/24");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));   // mid-rebuild
        net->close();
        run("ip link del usb0");
    }
    check(true, "survived five close-during-rebuild cycles");
}

void scenario_many_sessions() {
    section("several sessions in one process");
    std::vector<std::unique_ptr<uninet::Session>> all;
    for (int i = 0; i < 6; ++i)
        all.push_back(uninet::Session::join("N" + std::to_string(i), fast("reconnect-many")));
    int connected = 0;
    for (auto& s : all) if (s->connected()) ++connected;
    check(connected == 6, "all six joined");

    add_iface("usb0", "192.168.60.1/24");
    check(wait_until([&] {
              for (auto& s : all) if (s->transport().reconnect_count() == 0) return false;
              return true;
          }, 12.0),
          "every session rebuilt, not just the first");
    int still = 0;
    for (auto& s : all) if (s->connected()) ++still;
    check(still == 6, "all six still connected");
    for (auto& s : all) s->close();
    check(true, "all closed cleanly");
    run("ip link del usb0");
}

void scenario_no_network_at_all() {
    section("no network at all");
    run("ip link set eth0 down");
    auto net = uninet::Session::join("Stranded", fast("reconnect-none"));
    // Joining with no network must not crash or hang; it may or may not report
    // connected, but it must answer.
    check(true, "join returned with no network present");
    const uint64_t settled = net->transport().reconnect_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    check(net->transport().reconnect_count() - settled <= 1,
          "did not thrash while there was nothing to move to");
    check(!net->describe().empty(), "describe() still answers");
    net->close();
    check(true, "closed cleanly with no network");
    run("ip link set eth0 up");
}

}  // namespace

int main() {
    if (!run("ip link add uninet-probe type dummy >/dev/null 2>&1")) {
        std::printf("SKIP: cannot create interfaces here.\n"
                    "Run it through scripts/test-reconnect.sh, which provides a\n"
                    "private network namespace.\n");
        return 0;
    }
    run("ip link del uninet-probe >/dev/null 2>&1");

    run("ip link set lo up");
    add_iface("eth0", "192.168.50.1/24");

    scenario_interface_moves();
    scenario_peers_keep_talking();
    scenario_blob_survives();
    scenario_flapping();
    scenario_close_during_change();
    scenario_many_sessions();
    scenario_no_network_at_all();

    std::printf("\n%s (%d failure%s)\n", failures ? "FAIL" : "PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
