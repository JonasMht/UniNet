// UniNet — the whole library in one call.
//
//     auto net = uninet::Session::join("OR Headset");
//     net->subscribe("domain.D1", [](const uninet::Envelope& e) { ... });
//     net->publish("domain.D1", data);
//
// That is the complete setup. No address, no port, no broker, no config file,
// no server to install. Start the same program on another machine on the same
// network and the two find each other in about a second.
//
// Everything underneath stays reachable — node() and transport() expose the
// full surface — but nothing below this line is required to use UniNet.
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
    std::string app;     // "ThermoNavServer"

    // The one setting that ever needs changing: devices only see devices in the
    // same realm. Use it to keep a development machine out of a live session, or
    // to run two independent setups on one hospital network.
    std::string realm = "uninet";

    // Advanced, and normally left alone. `interface` matters only on a machine
    // with several networks, where discovery could otherwise pick the wrong one.
    int         port = 5670;
    // See ZyreConfig::iface — deliberately not called `interface`, which is a
    // macro in the Windows headers.
    std::string iface;

    Compression compression = DEFAULT_COMPRESSION;

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

    // ── sending ──
    // `dst` empty broadcasts to every device; otherwise the message is sent to
    // that one peer's uuid and nobody else receives it.
    // False when the message could not be sent (not on the network). Ignoring
    // the result is fine for fire-and-forget telemetry; check it when the
    // message matters.
    bool publish(const std::string& subject, Cbor data, const std::string& dst = "");
    // Same, from JSON text — the identical bytes reach the wire either way.
    bool publish_json(const std::string& subject, const std::string& json,
                      const std::string& dst = "");

    // ── receiving ──
    // `subject` is exact, or ends in ">" to match everything below it
    // ("thermonav.v1.>"). ">" alone matches everything.
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
    const std::string& uuid() const;   // address other devices publish to
    // One plain sentence for a status bar: no jargon, no addresses.
    std::string describe() const;

    // ── the full surface, when the one-call layer is not enough ──
    Node& node();
    ZyreTransport& transport();

private:
    Session();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace uninet
