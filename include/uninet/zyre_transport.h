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
// Threading: two background threads. One owns the Zyre socket and runs the
// event loop; it never runs application code, so it is always free to read the
// network. The other is the delivery thread, and every subscription and peer
// callback runs there, one at a time, in arrival order. Public methods are safe
// from any thread.
//
// A handler that blocks therefore costs latency and queue occupancy rather than
// lost messages -- see ZyreConfig's "keeping the network thread free" and
// delivery_stats(). Handlers should still return promptly; a handler that
// blocks for minutes will overflow any finite queue.
#pragma once

#include "uninet/peer.h"
#include "uninet/transport.h"

#include <cstdint>
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

    // Which network to discover on. Empty, the default, means every network at
    // once: Wi-Fi, wired, a USB tether and a VPN together. The beacon goes out
    // as a directed broadcast on each LAN and to every address of each
    // point-to-point subnet (WireGuard, OpenVPN tun), so devices find each other
    // over a VPN with no rendezvous node and no address to configure. This needs
    // the Zyre UniNet builds itself; with a system Zyre it falls back to the one
    // best network. Name an interface ("eth0", "192.168.1.10") to keep
    // discovery on that network only, e.g. to stay off a hospital LAN.
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

    // ── discovery through a rendezvous node ──
    // A VPN needs none of this: see `iface`. Gossip is for what the beacon can
    // never reach: a device behind a port forward (adb reverse), a routed
    // network, a cloud host. One node binds a rendezvous endpoint and the others
    // connect to it, so no broadcast is involved at all.
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

    // ── surviving a network change ──
    // Watch the machine's networks and rebuild the node when the one in use
    // goes away or is replaced: Wi-Fi dropping, moving between access points,
    // a cable pulled, a phone tethered or unplugged, a laptop waking up.
    //
    // Without this a session stays bound to an interface that no longer exists.
    // Nothing errors, nothing reconnects, and the device is simply deaf and
    // invisible until the application is restarted - which is the state ZRE
    // leaves it in, because the interface is chosen once when the beacon starts.
    //
    // Rebuilding means a new ZRE identity, so peers see this device leave and
    // rejoin. That is honest: from their side it did.
    bool auto_reconnect = true;
    // How often to look. Cheap (an interface enumeration), and 2 s is well
    // inside the 30 s a peer takes to expire us, so the far side usually never
    // notices the gap.
    int  reconnect_poll_ms = 2000;

    // ── keeping the network thread free ──
    // Subscription handlers do NOT run on the thread that reads the network.
    // They run on one dedicated delivery thread, fed by a bounded queue, and
    // the network thread does nothing but drain Zyre into that queue.
    //
    // WHY. The network thread is the only reader of Zyre's event outbox. Every
    // millisecond it spends inside an application handler is a millisecond it
    // is not reading, and the pipe behind it is finite: past its high-water
    // mark, ZeroMQ stops accepting, Zyre's own node thread blocks trying to
    // hand events over, its inbound router fills, and messages are DROPPED on
    // the far side of a link that reports itself perfectly healthy. Nothing
    // errors. The receiver simply misses messages.
    //
    // That is not a hypothetical. A handler in an embedded interpreter has to
    // take the host's lock (the GIL in Python) before it can run at all, so a
    // busy UI thread stalls delivery for as long as it stays busy -- and a
    // stall of seconds is ordinary when the UI is computing. Handlers running
    // on the network thread turn that into lost packets; handlers running here
    // turn it into queue occupancy, which is bounded, counted, and visible in
    // delivery_stats().
    //
    // Ordering and concurrency are unchanged: one queue, one thread, so
    // handlers still run one at a time, in arrival order, exactly as they did
    // when the network thread ran them. Peer found/lost callbacks go through
    // the same queue, so "peer arrived" cannot overtake that peer's messages.
    bool deliver_on_network_thread = false;

    // How much undelivered traffic the queue may hold. 0 means "no cap" for
    // that dimension -- an unbounded queue, which trades dropped messages for
    // unbounded memory.
    size_t max_delivery_bytes = 256u * 1024u * 1024u;   // 256 MiB
    size_t max_delivery_messages = 0;                   // no count cap

    // What happens at the cap: the network thread waits up to this long for the
    // handlers to make room, and only then discards the oldest entry.
    //
    // WHY BOTH, AND IN THIS ORDER. The two failure modes want opposite things
    // and neither policy alone is correct:
    //
    //   * A slower-but-progressing consumer -- a multi-gigabyte Blob arriving
    //     faster than it can be written -- needs BACKPRESSURE. Dropping there
    //     corrupts the transfer ("chunk out of order"), and the drop is not
    //     even necessary: the consumer is keeping up, just not instantly.
    //     Waiting hands the pressure back down the TCP connection to the
    //     sender, which is what a reliable transfer is built on.
    //
    //   * A consumer that has STOPPED -- a UI handler wedged behind a lock that
    //     will not come back -- must not be waited for forever. Blocking there
    //     blocks the network thread, and then discovery, presence and every
    //     other subject stop with it: the whole node goes deaf to fix one
    //     subscriber.
    //
    // Waiting first and dropping second serves both: real congestion drains and
    // loses nothing, a dead consumer costs one bounded stall and then keeps the
    // node alive, and delivery_stats() says which of the two happened.
    //
    // Set to 0 to never wait (drop as soon as the cap is reached), which is the
    // right choice only if every subscriber is known to be lossy-tolerant.
    int delivery_block_ms = 5000;
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

// What kind of link an interface is, judged by its name ("wlan0", "docker0").
LinkKind link_kind(const std::string& interface_name);

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

    // Whether the one-network fallback would choose this link. False for
    // loopback, container bridges and VPNs. Discovery on every network, the
    // default, still reaches a VPN, by sweeping its subnet.
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
    // By value, and guarded: a network rebuild replaces this string on the
    // network thread, so handing out a reference to it would be a race and a
    // dangling one the moment the string reallocates.
    std::string uuid() const;
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

    // Fired after the node has been rebuilt on a new network, on the network
    // thread. The ZRE identity has changed by then, so anything that cached
    // uuid() must re-read it; Session does exactly that.
    void on_reconnected(std::function<void()> cb);

    // How many times the network has been rebuilt under this session. Useful in
    // a status line, and in a test: it is the only externally visible proof
    // that a dropout was survived rather than never noticed.
    uint64_t reconnect_count() const;

    // ── the delivery queue ──
    // What the network thread has handed over and what the handlers have done
    // with it. This is the one place a "we are losing messages" report can be
    // settled without a packet capture: `dropped` is non-zero only when this
    // application's own handlers could not keep up, and `peak_queued` says how
    // close a run came to that.
    struct DeliveryStats {
        uint64_t queued = 0;          // waiting for a handler right now
        uint64_t queued_bytes = 0;    // their payload bytes
        uint64_t peak_queued = 0;     // high-water mark since connect()
        uint64_t peak_queued_bytes = 0;
        uint64_t delivered = 0;       // handed to the handlers
        uint64_t dropped = 0;         // evicted at the cap, never delivered
        // How long the network thread has spent waiting for the handlers to
        // make room, in microseconds, over the life of the session. Non-zero
        // means the queue reached its cap and backpressure did its job; a
        // large value with dropped == 0 is a healthy loaded transfer, and the
        // signal to raise max_delivery_bytes if the stall itself is a problem.
        uint64_t blocked_us = 0;
        // Longest single handler run seen, in microseconds. A handler that
        // blocks for a second is the reason a queue backs up, and this names
        // it without a profiler.
        uint64_t slowest_handler_us = 0;
        // False when handlers run on the network thread (the escape hatch in
        // ZyreConfig), which is where dropped-on-the-wire behaviour comes back.
        bool threaded = true;
    };
    DeliveryStats delivery_stats() const;

    // Resize the queue while it is running. 0 means "no cap" for that
    // dimension. Raising it costs nothing until the traffic arrives; lowering
    // it below the current occupancy evicts oldest-first, counted as dropped.
    void set_delivery_limits(size_t max_bytes, size_t max_messages);

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
