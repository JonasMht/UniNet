// UniNet: discovery on every network at once.
//
// Zyre sends its UDP beacon on ONE interface, and only as a broadcast. A machine
// on Wi-Fi and a VPN at the same time is therefore discoverable on one of them,
// and a VPN is never discoverable at all: WireGuard and other point-to-point
// links carry no broadcast. This sends the node's beacon on every network
// instead: as a directed broadcast on each LAN, and to each address of the
// subnet on each point-to-point link. The receiving Zyre listens on every
// interface and cannot tell a unicast beacon from a broadcast one, so nothing
// else changes and no node has to be told where any other node is.
//
// Internal: not part of the public API.
#pragma once

#include "uninet/zyre_transport.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace uninet::detail {

// One IPv4 network this machine is on. Addresses in host byte order.
struct Link {
    std::string name;
    uint32_t    address = 0;
    uint32_t    netmask = 0;
    uint32_t    broadcast = 0;         // 0 when the link has none
    bool        point_to_point = false;
};

// Every IPv4 link that is up, loopback excluded, point-to-point links INCLUDED.
// czmq's ziflist leaves point-to-point links out, and those are the VPNs.
std::vector<Link> ipv4_links();

// How the beacon reaches the nodes on one link.
struct BeaconRoute {
    Link                  link;
    std::vector<uint32_t> destinations;   // empty when the link is skipped
    std::string           how;            // "broadcast", "sweep of N", or why skipped
};

// Point-to-point subnets larger than this are not swept: a /16 corporate VPN
// would take 65534 datagrams a second.
constexpr int MAX_SWEEP_HOSTS = 1022;   // a /22

// Where the beacon goes on each of `links`: the directed broadcast of a LAN, or
// every other host address of a point-to-point subnet no larger than a /22.
// Container bridges are skipped; no UniNet device sits behind one.
std::vector<BeaconRoute> beacon_routes(const std::vector<Link>& links);

// The 22-byte ZRE v2 beacon: "ZRE", version 1, the node's 16-byte uuid, and the
// TCP port of its mailbox in network byte order. Empty if `uuid_hex` is not 32
// hex digits.
std::vector<uint8_t> zre_beacon(const std::string& uuid_hex, uint16_t mailbox_port);

// Sends a beacon along beacon_routes(ipv4_links()) once per interval, from its
// own thread, re-reading the links every time so a network that comes up later
// (a VPN connecting, a phone being tethered) is covered within one interval.
class BeaconFanout {
public:
    BeaconFanout(std::vector<uint8_t> beacon, uint16_t udp_port, int interval_ms);
    ~BeaconFanout();

    BeaconFanout(const BeaconFanout&) = delete;
    BeaconFanout& operator=(const BeaconFanout&) = delete;

private:
    void run();

    std::vector<uint8_t>    beacon_;
    uint16_t                udp_port_;
    int                     interval_ms_;
    std::mutex              mu_;
    std::condition_variable wake_;
    bool                    stop_ = false;
    std::thread             thread_;
};

// A TCP port nothing is bound to right now, or 0. Zyre's beacon has to carry
// the mailbox port, and with the stable API Zyre never says which port it
// picked, so the port is chosen here and handed to it.
uint16_t free_tcp_port();

}  // namespace uninet::detail
