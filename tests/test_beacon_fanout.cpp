// UniNet: where the discovery beacon goes on each kind of network, and what it
// contains. The sending itself is covered by the network tests; this pins the
// routing rules, which decide whether a VPN peer is ever found.
#include "beacon_fanout.h"

#include <cstdio>

using namespace uninet::detail;

static int failures = 0;
#define CHECK(cond) do { if(!(cond)){ std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while(0)

static uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) { return (a << 24) | (b << 16) | (c << 8) | d; }

static Link link(const char* name, uint32_t address, int prefix, uint32_t broadcast, bool p2p) {
    Link l;
    l.name = name;
    l.address = address;
    l.netmask = prefix == 0 ? 0 : ~0u << (32 - prefix);
    l.broadcast = broadcast;
    l.point_to_point = p2p;
    return l;
}

static void test_lan_gets_its_directed_broadcast() {
    auto routes = beacon_routes({link("wlan0", ip(172, 29, 65, 104), 16, ip(172, 29, 255, 255), false)});
    CHECK(routes.size() == 1);
    CHECK(routes[0].destinations.size() == 1);
    CHECK(routes[0].destinations[0] == ip(172, 29, 255, 255));
    CHECK(routes[0].how == "broadcast");
}

static void test_vpn_is_swept_except_ourselves() {
    // WireGuard: point-to-point, no broadcast. Named like ours, not "wg0",
    // so this must not depend on the interface name.
    auto routes = beacon_routes({link("Carnation", ip(10, 0, 0, 4), 24, 0, true)});
    CHECK(routes.size() == 1);
    const auto& d = routes[0].destinations;
    CHECK(d.size() == 253);                         // 254 hosts, minus our own
    CHECK(d.front() == ip(10, 0, 0, 1));
    CHECK(d.back() == ip(10, 0, 0, 254));           // never the network or broadcast address
    for (uint32_t a : d) CHECK(a != ip(10, 0, 0, 4));
    CHECK(routes[0].how == "sweep of 253");
}

static void test_what_is_not_swept() {
    auto routes = beacon_routes({
        link("tun0", ip(10, 8, 0, 2), 16, 0, true),                          // too large
        link("wg1", ip(10, 9, 0, 2), 32, 0, true),                           // nobody else on it
        link("docker0", ip(172, 17, 0, 1), 16, ip(172, 17, 255, 255), false),// container bridge
    });
    CHECK(routes.size() == 3);
    for (const auto& r : routes) CHECK(r.destinations.empty());
    CHECK(routes[0].how.find("too large") != std::string::npos);
    CHECK(routes[2].how.find("container") != std::string::npos);
}

static void test_largest_sweep() {
    auto routes = beacon_routes({link("wg0", ip(10, 0, 0, 1), 22, 0, true)});
    CHECK(routes[0].destinations.size() == (size_t)MAX_SWEEP_HOSTS - 1);
}

static void test_beacon_bytes() {
    auto b = zre_beacon("28bbbe581135927bf21349f697e1c99d", 49152);
    CHECK(b.size() == 22);
    CHECK(b[0] == 'Z' && b[1] == 'R' && b[2] == 'E' && b[3] == 0x01);
    CHECK(b[4] == 0x28 && b[19] == 0x9d);
    CHECK(b[20] == 0xC0 && b[21] == 0x00);        // 49152, network byte order
    CHECK(zre_beacon("not a uuid", 1).empty());
    CHECK(zre_beacon("zzbbbe581135927bf21349f697e1c99d", 1).empty());
}

int main() {
    test_lan_gets_its_directed_broadcast();
    test_vpn_is_swept_except_ourselves();
    test_what_is_not_swept();
    test_largest_sweep();
    test_beacon_bytes();
    std::printf(failures ? "%d FAILURE(S)\n" : "beacon fan-out: all checks passed\n", failures);
    return failures ? 1 : 0;
}
