// UniNet: the transport. Brokerless peer-to-peer over ZeroMQ's ZRE protocol
// (zeromq/zyre): nodes announce themselves with a UDP beacon, connect directly
// to each other over TCP, and form a group. There is no server to install, no
// broker to start, and no address for anyone to type.
//
// A brokered bus would mean a server running somewhere, every peer carrying that
// server's address, and a room change or a new laptop turning into a source edit
// in three languages. ZRE removes the broker and the address together.
//
// How UniNet's concepts map onto ZRE:
//
//   realm      -> the ZRE group every node joins. Two realms on one network
//                 never see each other, so a dev laptop cannot join a live
//                 clinical session by accident.
//   publish    -> SHOUT to the group (broadcast), or WHISPER to one peer when
//                 the message is addressed (dst_uuid). Unicast is a real
//                 send-to-one, not a broadcast everyone filters.
//   subject    -> travels as its own frame beside the payload, so a receiver
//                 matches subscriptions without decoding anything.
//   peers      -> ZRE ENTER/EXIT events, surfaced as Peer callbacks.
//
// Echo suppression comes free: ZRE never delivers a node its own SHOUT.
//
// Threading: one background thread owns the Zyre socket and runs the event
// loop. Public methods are safe from any thread. Callbacks fire on that
// background thread: do not block them.
#pragma once

#include "uninet/peer.h"
#include "uninet/transport.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace uninet {

struct ZyreConfig {
    // The ZRE group this node joins. Peers only ever see peers in the same
    // realm; it is the one knob that separates two setups sharing a network.
    std::string realm = "uninet";

    // UDP beacon port. The ZRE default; change it only to run fully independent
    // clusters on one physical network.
    int port = 5670;

    // Which network interface to beacon on. Empty lets CZMQ choose, which is
    // right on a single-homed machine. On a box with several interfaces (wired
    // to the navigation switch AND on hospital Wi-Fi) name the one you mean -
    // "eth0" or "192.168.1.10", or discovery may pick the wrong network.
    //
    // NOT named `interface`: <objbase.h> defines that as a macro on Windows
    // (#define interface struct), which breaks every translation unit that
    // includes a Windows header before this one.
    std::string iface;

    // A peer that has said nothing for this long is pinged (evasive), then
    // declared gone (expired). The defaults declare a yanked cable in ~30 s;
    // lower them for a room where a device vanishing should be noticed fast.
    int evasive_ms = 5000;
    int expired_ms = 30000;

    // ── discovery over a link with no multicast ──
    // The UDP beacon above needs peers to share a broadcast domain. Some links
    // do not provide one: a USB-tethered device reached through a port forward,
    // a VPN, a routed network, a cloud host. For those, ZRE offers gossip
    // discovery: one node binds a rendezvous endpoint and the others connect to
    // it, so no multicast is involved at all.
    //
    // Setting either of these switches this node from beacon to gossip mode.
    // At least one node in the group must bind; every other node connects.
    //
    //   rendezvous:  cfg.gossip_bind    = "tcp://*:5670";
    //   the others:  cfg.gossip_connect = "tcp://192.168.1.10:5670";
    //
    // Gossip carries only the introductions. Peers still open direct TCP
    // connections to each other afterwards, so every node's `endpoint` has to be
    // reachable from every other node. On a link where that is not automatic
    // (a port-forwarded tunnel), set `endpoint` and `advertised_endpoint` to the
    // address the far side should actually dial.
    std::string gossip_bind;
    std::string gossip_connect;

    // This node's own data endpoint. Only meaningful in gossip mode, where it
    // replaces the ephemeral port ZRE would otherwise pick. Empty keeps the
    // default behaviour.
    std::string endpoint;
    // What to tell other nodes this node's endpoint is, when that differs from
    // what it binds (behind a NAT or a forwarded port).
    //
    // Requires a Zyre built with -DENABLE_DRAFTS, which distribution packages
    // usually are not. On a stable-only build this is ignored and last_error()
    // says so; gossip_bind/gossip_connect and endpoint all work either way.
    std::string advertised_endpoint;

    // Advertised to every peer at discovery time and readable as Peer::headers.
    // "role", "app" and "host" get accessors on Peer; anything else is yours.
    std::map<std::string, std::string> headers;
};

enum class LinkKind {
    Unknown,
    Wired,      // ethernet
    Wireless,   // Wi-Fi
    Tethered,   // a phone sharing its connection over USB: rndis0, usb0, enp0s20u1
    Virtual,    // docker0, br-*, veth*, virbr*: real addresses, no other devices
    Vpn,        // tun*, tap*, wg*: point-to-point, no broadcast domain
    Loopback,
};

const char* link_kind_name(LinkKind kind);

// One network this machine could discover on.
struct Interface {
    std::string name;        // "wlan0", "eth0", "Wi-Fi"
    std::string address;     // IPv4 address on that interface
    std::string broadcast;   // where the discovery beacon would be sent
    std::string netmask;
    LinkKind    kind = LinkKind::Unknown;

    // True for the address ranges reserved for private networks (RFC1918) and
    // link-local (169.254/16). A hotspot, a USB tether and a home or lab LAN
    // are all private; a campus or hospital address usually is not.
    bool is_private() const;

    // Whether discovery is worth attempting here at all. False for loopback,
    // container bridges and point-to-point VPN links, none of which have a
    // broadcast domain with other UniNet devices on it.
    bool is_discoverable() const;
};

// Every usable IPv4 interface, loopback excluded.
//
// Discovery binds ONE of these. Which one is not always the one you want: a
// machine with a wired network, a VPN and a few docker bridges has several, and
// a beacon sent on the wrong one reaches nobody while everything reports
// healthy. That failure is invisible without a list like this, which is why
// uninet-discover prints it when it finds nothing. Set `iface` to pick.
std::vector<Interface> local_interfaces();

// The interface discovery should use when the application does not name one.
//
// czmq's default is "the first one the OS lists", which on a developer machine
// is regularly a docker bridge or the wired port while the device is on Wi-Fi.
// This prefers, in order: a USB-tethered phone (it was plugged in on purpose,
// so it is almost certainly the intent), then Wi-Fi, then wired, and never a
// container bridge, a VPN or loopback. Returns an empty name when nothing is
// usable, which is a real state on a machine with no network.
Interface best_interface(const std::vector<Interface>& candidates);


class ZyreTransport : public Transport {
public:
    using PeerCallback = std::function<void(const Peer&)>;

    // `name` is what other devices will show for this one. Nothing else is
    // required, no address, no port, no peer list.
    explicit ZyreTransport(std::string name, ZyreConfig cfg = {});
    ~ZyreTransport() override;

    ZyreTransport(const ZyreTransport&) = delete;
    ZyreTransport& operator=(const ZyreTransport&) = delete;

    // ── Transport ──
    bool connect() override;          // start beaconing and join the realm
    void disconnect() override;
    bool connected() const override;
    bool publish(const std::string& subject, const uint8_t* data, size_t len) override;
    bool publish_to(const std::string& peer_uuid, const std::string& subject,
                    const uint8_t* data, size_t len) override;
    bool can_address() const override { return true; }
    void subscribe(const std::string& subject, MessageHandler handler) override;
    void unsubscribe(const std::string& subject) override;
    std::string name() const override { return "zyre"; }

    // ── presence ──
    // Everyone currently on the network, in this realm. Never includes self.
    std::vector<Peer> peers() const;
    // Fired as devices arrive and leave. Set these before connect() to catch
    // the peers that are already present when this node starts.
    void on_peer_found(PeerCallback cb);
    void on_peer_lost(PeerCallback cb);

    // This node's ZRE identity: the uuid other peers address it by.
    const std::string& uuid() const;
    const std::string& node_name() const;

    // Advertise another key/value. Must be called before connect(): ZRE sends
    // headers once, with the discovery beacon. Returns false (and sets
    // last_error) if called afterwards, when it could not take effect.
    bool set_header(const std::string& key, const std::string& value);

    // Why connect() failed, when it did.
    std::string last_error() const;

    // The network discovery settled on, once connected. Empty in gossip mode
    // (no beacon) and when the application named an interface itself. Worth
    // showing in a status line: "which network am I actually on" is otherwise
    // unanswerable from inside the application, and getting it wrong is the
    // most common reason two devices never see each other.
    Interface chosen_interface() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The ZeroMQ/Zyre version UniNet is running against, e.g. "zyre 2.0.1 / czmq
// 4.2.2 / zmq 4.3.5". Useful in a bug report.
std::string zyre_version_string();

// This machine's hostname, sanitized for display. Empty if it cannot be read.
// Advertised by Session as the "host" header, so a device list can show which
// machine a peer is running on.
std::string local_hostname();

// What kind of network an interface is. Discovery treats these very
// differently, and guessing from the name alone is how a beacon ends up on a
// container bridge that routes nowhere.

}  // namespace uninet
