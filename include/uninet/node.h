// UniNet: high-level peer. A Node owns a UUID, a Transport, and a default
// compression. It encodes Envelopes, suppresses self-echo, honors dst_uuid
// targeting, and reconnects with backoff. This is the single place the protocol
// lives, so no application has to reimplement it.
#pragma once

#include "uninet/codec.h"
#include "uninet/transport.h"

#include <functional>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace uninet {

class Node {
public:
    // Delivered to the application after framing/echo/dst filtering.
    using DataHandler = std::function<void(const Envelope& env)>;

    Node(std::string name, Transport* transport, Compression compress = DEFAULT_COMPRESSION);

    // Same, with a caller-supplied UUID instead of a freshly generated one. Used
    // when an identity must outlive one Node: a Session that reconnects onto a
    // discovered bus builds a new Node, and peers must not see that as a
    // different device appearing. An empty `uuid` falls back to make_uuid(name).
    Node(std::string name, std::string uuid, Transport* transport,
         Compression compress = DEFAULT_COMPRESSION);

    const std::string& uuid() const { return uuid_; }
    const std::string& name() const { return name_; }
    Transport* transport() const { return transport_; }

    bool connect();
    bool connected() const;

    // Retry connection with exponential backoff. attempts<=0 means "until connected".
    // Loopback always succeeds on the first try.
    bool retry_connect(int attempts, double base_sleep_s = 0.1);

    // Publish an arbitrary message payload. dst_uuid="" broadcasts; otherwise the
    // message goes to that peer alone when the transport can address one
    // (ZyreTransport can), and falls back to a broadcast that receivers filter
    // on dst_uuid when it cannot (loopback).
    // Returns false when the message could not be handed to the transport -
    // not connected, or the transport refused it. A void return made a publish
    // during a network outage indistinguishable from a successful one.
    bool publish(const std::string& subject, Cbor data, const std::string& dst_uuid = "");

    // Subscribe to a subject (exact or wildcard). The handler receives accepted
    // envelopes only (own echoes and non-matching dst_uuids are filtered).
    void subscribe(const std::string& subject, DataHandler handler);

private:
    std::string name_;
    std::string uuid_;
    Transport* transport_;
    Compression compress_;
    // The internal ">" subscription, established once the transport is up.
    // Guarded because subscribe() may be called from several threads at once:
    // unsynchronised, they all saw watching_ == false and each installed a
    // duplicate subscription, so one message was delivered N times. The window
    // is wide (it spans the transport's own mutex and a push_back), so this was
    // a routine hit, not a rare one.
    std::atomic<bool> watching_{false};
    std::mutex watch_mu_;

    std::mutex handlers_mu_;
    std::vector<std::pair<std::string, DataHandler>> handlers_;

    void ensure_watching_();   // create the internal ">" subscription once the transport is up
    void on_raw_(const std::string& subject, const Bytes& payload);
};

// Build a UUID: "<name>_<YYYYmmdd-HHMMSS>_<rand>". Lighter-collision than the
// 1-in-1000 random the Slicer peer uses today.
std::string make_uuid(const std::string& name);

}  // namespace uninet
