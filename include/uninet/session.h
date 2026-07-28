// UniNet: the whole library in one call.
//
//     auto net = uninet::Session::join("Headset");
//     net->subscribe("domain.D1", [](const uninet::Envelope& e) { ... });
//     net->publish("domain.D1", data);
//
// That is the complete setup. No address, no port, no broker, no config file,
// no server to install. Start the same program on another machine on the same
// network and the two find each other in about a second.
//
// Everything underneath stays reachable: node() and transport() expose the
// full surface, but nothing below this line is required to use UniNet.
#pragma once

#include "uninet/node.h"
#include "uninet/zyre_transport.h"

#include <memory>
#include <string>
#include <vector>

namespace uninet {

struct SessionConfig {
    // Shown to other devices in a peer list. Both optional, both free-form.
    std::string role;    // "server" / "headset" / "viewer"
    std::string app;     // your application's name

    // The one setting that ever needs changing: devices only see devices in the
    // same realm. Use it to keep a development machine out of a live session, or
    // to run two independent setups on one physical network.
    std::string realm = "uninet";

    // Discovery over a link with no multicast: a USB-tethered device reached
    // through a port forward, a VPN, a routed network. One node binds a
    // rendezvous endpoint, the others connect to it. See ZyreConfig for the
    // details and for the endpoint caveat on tunnelled links.
    std::string gossip_bind;      // e.g. "tcp://*:5670" on the rendezvous node
    std::string gossip_connect;   // e.g. "tcp://127.0.0.1:5670" on the others
    std::string endpoint;             // this node's data endpoint (gossip mode)
    std::string advertised_endpoint;  // what to tell peers, if it differs

    // Advanced, and normally left alone. `iface` matters only on a machine
    // with several networks, where discovery could otherwise pick the wrong one.
    int         port = 5670;
    // See ZyreConfig::iface: deliberately not called `interface`, which is a
    // macro in the Windows headers.
    std::string iface;

    Compression compression = DEFAULT_COMPRESSION;

    // Follow the machine's networks: rebuild when the one in use goes away or a
    // better one appears. Wi-Fi dropping, moving between access points, a cable
    // pulled, a phone tethered, a laptop waking up.
    //
    // On by default because the alternative is silent: ZRE binds an interface
    // once, and without this a session stays attached to one that no longer
    // exists, deaf and invisible, reporting no error. Turn it off only if
    // something else in your application owns reconnection.
    bool auto_reconnect = true;
    int  reconnect_poll_ms = 2000;

    // ── bridging separate networks ──
    // A machine with more than one network can put a node on each and pass
    // messages between them, so devices that cannot see each other still talk:
    // a phone on Wi-Fi and a headset on a USB tether, a laptop straddling a lab
    // switch and a hotspot.
    //
    // ON by default. Be aware of what that means: it JOINS networks that were
    // separate. Traffic from one becomes visible on the other, for every device
    // in the same realm. On a machine that sits between a hospital network and
    // a private one, that may not be what you want, and allow_bridging(false)
    // is a single setting.
    //
    // Only networks that discovery would use are bridged: never a container
    // bridge, a VPN tunnel or loopback. A realm still isolates completely, so
    // two setups sharing a bridged machine remain invisible to each other.
    bool allow_bridging = true;

    // Include a USB-tethered phone or headset among the networks bridged.
    // Separate from allow_bridging because a tether is the case people most
    // want and the least likely to be sensitive: it is a cable someone plugged
    // in deliberately, not a network the machine happens to be on.
    bool allow_usb = true;

    // Where the helper lives. Empty looks next to the running executable and
    // then on PATH. One process per extra network is not an implementation
    // detail that could be avoided: czmq's interface selection is a
    // process-global, so a second node in this process cannot be put on a
    // different network. See tools/uninet_bridge.cpp.
    std::string bridge_helper;

    // Extra key/value advertised to every peer, readable via Peer::header().
    std::map<std::string, std::string> headers;
};

class Session {
public:
    using PeerCallback = ZyreTransport::PeerCallback;

    // Join the network. Never returns null and never throws. If the network is
    // unavailable the session still exists and connected() reports false, so a
    // caller has one thing to check rather than an exception to handle.
    static std::unique_ptr<Session> join(const std::string& name, SessionConfig cfg = {});

    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Leave the network and release the transport. Idempotent, and safe to call
    // from a shutdown hook. The destructor calls it, but relying on destruction
    // alone is not enough everywhere: a garbage-collected host (Python, C#) may
    // finalize this object after the ZeroMQ context has already been torn down,
    // and closing a socket after that aborts inside czmq. Call it explicitly
    // when you control the shutdown order.
    void close();
    // False once close() has run.
    bool open() const;

    // ── sending ──
    // `dst` empty broadcasts to every device; otherwise the message is sent to
    // that one peer's uuid and nobody else receives it.
    // False when the message could not be sent (not on the network). Ignoring
    // the result is fine for fire-and-forget telemetry; check it when the
    // message matters.
    /// @param mid  carry an existing message id instead of minting one. Only a
    ///             relay needs this: preserving the id across a hop is what
    ///             lets the far side recognise a message that has come back
    ///             around, and two identical publishes are otherwise
    ///             indistinguishable from one that looped.
    bool publish(const std::string& subject, Cbor data, const std::string& dst = "",
                 const std::string& mid = "");
    // Same, from JSON text: the identical bytes reach the wire either way.
    bool publish_json(const std::string& subject, const std::string& json,
                      const std::string& dst = "");

    // ── receiving ──
    // `subject` is exact, or ends in ">" to match everything below it
    // ("sensors.>"). ">" alone matches everything.
    //
    // LIFETIME. The handler runs on the network thread and keeps running until
    // the session is closed. Anything it captures by reference must outlive the
    // session:
    //
    //     std::vector<Msg> received;              // declared BEFORE the session
    //     auto net = Session::join("Viewer");     // so it is destroyed AFTER it
    //     net->subscribe("t.>", [&](auto& e) { received.push_back(...); });
    //
    // Declared the other way round, `received` is destroyed first (locals go in
    // reverse order) and a message arriving in that window writes to freed
    // memory. Calling close() before the captured state goes away is the
    // explicit way to be sure.
    void subscribe(const std::string& subject, Node::DataHandler handler);

    // ── who else is here ──
    std::vector<Peer> peers() const;
    // Fires for every device already present as well as those that arrive next,
    // so registration order does not change what you see.
    void on_peer_found(PeerCallback cb);
    void on_peer_lost(PeerCallback cb);

    // ── identity and state ──
    bool connected() const;
    const std::string& name() const;
    // By value: a network change replaces the identity underneath, so a
    // reference could dangle. See ZyreTransport::uuid().
    std::string uuid() const;   // address other devices publish to
    // One plain sentence for a status bar: no jargon, no addresses.
    std::string describe() const;

    // Why the last operation failed, when one did. Empty when healthy.
    std::string last_error() const;

    // ── the full surface, when the one-call layer is not enough ──
    // Both throw std::runtime_error once the session is closed.
    Node& node();
    ZyreTransport& transport();

    /// Number of extra networks this session is bridging onto. Zero when
    /// bridging is off, when there is only one usable network, or in gossip
    /// mode. Worth showing: it is the difference between "the other device is
    /// not there" and "it is there, on a network nothing is carrying".
    size_t bridge_count() const;

    /// Adopt a bridge this process did not start.
    ///
    /// Session starts a helper per extra INTERFACE by itself. This is for the
    /// cases that enumeration cannot cover: a second network identified by
    /// beacon port rather than interface, a helper started under a debugger, or
    /// one running on another machine and reached over tcp. `endpoint` is a
    /// ZeroMQ endpoint this session will BIND as a PAIR; the helper connects to
    /// it with --link.
    ///
    /// Returns false if the endpoint could not be bound.
    bool adopt_bridge(const std::string& label, const std::string& endpoint);

private:
    Session();
    void stop_bridges();
    void start_relay();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace uninet
