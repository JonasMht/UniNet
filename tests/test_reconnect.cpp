// Does UniNet survive the network changing under it?
//
// Discovery binds one interface when the beacon starts, and ZRE never rebinds.
// So Wi-Fi dropping, moving between access points, a cable pulled or a phone
// tethered used to leave a session bound to an interface that no longer exists:
// deaf and invisible, with nothing reported, until the application restarted.
//
// This test changes the machine's networks while a session is running and
// checks that it notices and rebuilds.
//
// It has to create and destroy interfaces, which normally means root. It does
// not here: the runner puts it in a private network namespace with `unshare
// -rn`, where an unprivileged user is root and the interfaces are synthetic and
// invisible to the rest of the machine. See scripts/test-reconnect.sh. Run
// outside such a namespace it skips rather than failing, because there it would
// be testing the developer's real network.
#include "uninet/zyre_transport.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s   %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

bool run(const std::string& cmd) { return std::system(cmd.c_str()) == 0; }

// Wait for a predicate rather than sleeping a fixed time: the supervisor polls,
// so a fixed sleep would either be flaky or slow.
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

}  // namespace

int main() {
    // Only meaningful where interfaces can be created. Anywhere else this would
    // be poking the real network, so decline rather than pretend.
    if (!run("ip link add uninet-probe type dummy >/dev/null 2>&1")) {
        std::printf("SKIP: cannot create interfaces here.\n"
                    "Run it through scripts/test-reconnect.sh, which provides a\n"
                    "private network namespace.\n");
        return 0;
    }
    run("ip link del uninet-probe >/dev/null 2>&1");

    run("ip link set lo up");
    run("ip link add eth0 type dummy");
    run("ip addr add 192.168.50.1/24 dev eth0");
    run("ip link set eth0 up");

    std::printf("start on a wired network only\n");
    uninet::ZyreConfig cfg;
    cfg.realm = "reconnect-check";
    cfg.reconnect_poll_ms = 300;      // the test should take seconds, not a minute
    uninet::ZyreTransport net("Mover", cfg);
    check(net.connect(), "joined");
    check(net.chosen_interface().name == "eth0",
          "picked eth0, got '" + net.chosen_interface().name + "'");
    check(net.reconnect_count() == 0, "no rebuild before anything changed");

    // Nothing has changed, so nothing should happen. A supervisor that rebuilds
    // on its own schedule would churn the network forever and be worse than
    // none at all; this is the check that catches it.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    check(net.reconnect_count() == 0, "still no rebuild while the network is stable");

    // A phone is tethered. It outranks a wired network, so this is now the
    // interface to be on.
    std::printf("\ntether a phone (usb0 appears)\n");
    run("ip link add usb0 type dummy");
    run("ip addr add 192.168.60.1/24 dev usb0");
    run("ip link set usb0 up");

    check(wait_until([&] { return net.reconnect_count() >= 1; }, 5.0),
          "noticed the new network and rebuilt");
    check(wait_until([&] { return net.chosen_interface().name == "usb0"; }, 5.0),
          "moved to usb0, now on '" + net.chosen_interface().name + "'");
    check(net.connected(), "still connected after the move");

    // The cable is pulled. The only remaining network is the wired one, so it
    // has to go back rather than staying bound to an interface that is gone.
    std::printf("\nunplug the phone\n");
    const uint64_t before = net.reconnect_count();
    run("ip link del usb0");

    check(wait_until([&] { return net.reconnect_count() > before; }, 5.0),
          "noticed the network disappear and rebuilt");
    check(wait_until([&] { return net.chosen_interface().name == "eth0"; }, 5.0),
          "fell back to eth0, now on '" + net.chosen_interface().name + "'");
    check(net.connected(), "still connected after falling back");

    // Losing everything is not a crash, and not a busy loop: with no network to
    // move to there is nothing to rebuild onto, so it should wait quietly.
    std::printf("\nlose every network\n");
    run("ip link set eth0 down");
    const uint64_t settled = net.reconnect_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    check(net.reconnect_count() - settled <= 1,
          "did not thrash while there was no network to move to");

    net.disconnect();
    check(true, "disconnected cleanly with the network gone");

    std::printf("\n%s (%d failure%s)\n", failures ? "FAIL" : "PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
