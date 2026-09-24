#include "beacon_fanout.h"

#include <czmq.h>

#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace uninet::detail {

namespace {

#if defined(_WIN32)
uint32_t parse_ipv4(const std::string& text) {
    in_addr a{};
    return inet_pton(AF_INET, text.c_str(), &a) == 1 ? ntohl(a.s_addr) : 0;
}
#endif

int prefix_length(uint32_t netmask) {
    int n = 0;
    while (netmask & 0x80000000u) { ++n; netmask <<= 1; }
    return n;
}

}  // namespace

std::vector<Link> ipv4_links() {
    std::vector<Link> out;
#if defined(_WIN32)
    // Windows lists adapters differently and czmq's ziflist already walks them.
    // It omits point-to-point adapters, so a VPN is not swept on Windows yet;
    // every LAN is still covered.
    for (const auto& i : local_interfaces()) {
        Link link;
        link.name      = i.name;
        link.address   = parse_ipv4(i.address);
        link.netmask   = parse_ipv4(i.netmask);
        link.broadcast = parse_ipv4(i.broadcast);
        if (link.address) out.push_back(link);
    }
#else
    ifaddrs* all = nullptr;
    if (getifaddrs(&all) != 0) return out;
    for (const ifaddrs* i = all; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        Link link;
        link.name    = i->ifa_name;
        link.address = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
        link.netmask = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
        link.point_to_point = (i->ifa_flags & IFF_POINTOPOINT) != 0;
        // On a point-to-point link ifa_broadaddr is the far end (ifa_dstaddr
        // shares its slot), not a broadcast address.
        if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr)
            link.broadcast = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr);
        out.push_back(link);
    }
    freeifaddrs(all);
#endif
    return out;
}

std::vector<BeaconRoute> beacon_routes(const std::vector<Link>& links) {
    std::vector<BeaconRoute> routes;
    for (const auto& link : links) {
        BeaconRoute route{link, {}, ""};
        const int prefix = prefix_length(link.netmask);
        const uint32_t hosts = prefix >= 31 ? 0u : (1u << (32 - prefix)) - 2u;

        if (link_kind(link.name) == LinkKind::Virtual) {
            route.how = "skipped: container bridge";
        } else if (link.broadcast && !link.point_to_point) {
            route.destinations.push_back(link.broadcast);
            route.how = "broadcast";
        } else if (hosts == 0) {
            route.how = "skipped: no other address on a /" + std::to_string(prefix);
        } else if (hosts > (uint32_t)MAX_SWEEP_HOSTS) {
            route.how = "skipped: /" + std::to_string(prefix) + " is too large to sweep";
        } else {
            const uint32_t network = link.address & link.netmask;
            for (uint32_t h = 1; h <= hosts; ++h)
                if (network + h != link.address) route.destinations.push_back(network + h);
            route.how = "sweep of " + std::to_string(route.destinations.size());
        }
        routes.push_back(std::move(route));
    }
    return routes;
}

std::vector<uint8_t> zre_beacon(const std::string& uuid_hex, uint16_t mailbox_port) {
    if (uuid_hex.size() != 32) return {};
    std::vector<uint8_t> beacon = {'Z', 'R', 'E', 0x01};
    for (size_t i = 0; i < 32; i += 2) {
        char* end = nullptr;
        const std::string pair = uuid_hex.substr(i, 2);
        const unsigned long byte = std::strtoul(pair.c_str(), &end, 16);
        if (end != pair.c_str() + 2) return {};
        beacon.push_back(static_cast<uint8_t>(byte));
    }
    beacon.push_back(static_cast<uint8_t>(mailbox_port >> 8));
    beacon.push_back(static_cast<uint8_t>(mailbox_port & 0xff));
    return beacon;
}

BeaconFanout::BeaconFanout(std::vector<uint8_t> beacon, uint16_t udp_port, int interval_ms)
    : beacon_(std::move(beacon)), udp_port_(udp_port), interval_ms_(interval_ms) {
    thread_ = std::thread([this] { run(); });
}

BeaconFanout::~BeaconFanout() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void BeaconFanout::run() {
    // czmq's helper, because it is the one portable way to get a UDP socket
    // allowed to broadcast (SO_BROADCAST, and WSAStartup already done on Windows).
    const SOCKET sock = zsys_udp_new(/*routable=*/false);
    if (sock == INVALID_SOCKET) return;

    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        lk.unlock();
        for (const auto& route : beacon_routes(ipv4_links())) {
            for (uint32_t destination : route.destinations) {
                sockaddr_in to{};
                to.sin_family      = AF_INET;
                to.sin_port        = htons(udp_port_);
                to.sin_addr.s_addr = htonl(destination);
                // Best effort, like the beacon it copies: a datagram that cannot
                // be sent now is sent again in one interval.
                sendto(sock, reinterpret_cast<const char*>(beacon_.data()), (int)beacon_.size(), 0,
                       reinterpret_cast<const sockaddr*>(&to), sizeof(to));
            }
        }
        lk.lock();
        wake_.wait_for(lk, std::chrono::milliseconds(interval_ms_), [this] { return stop_; });
    }
    lk.unlock();
    zsys_udp_close(sock);
}

uint16_t free_tcp_port() {
    const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return 0;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = 0;
    socklen_t len = sizeof(addr);
    uint16_t port = 0;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
        getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0)
        port = ntohs(addr.sin_port);
#if defined(_WIN32)
    closesocket(sock);
#else
    close(sock);
#endif
    return port;
}

}  // namespace uninet::detail
