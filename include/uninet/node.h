// UniNet — high-level peer. A Node owns a UUID, a Transport, and a default
// compression. It encodes Envelopes, suppresses self-echo, honors dst_uuid
// targeting, and reconnects with backoff. This is the single place the protocol
// lives — exactly what ThermoNavMR (Networking.cs), ThermoNavServer
// (networking.cpp), and ThermoNavSlicer (networking.py) each re-implement today.
#pragma once

#include "uninet/codec.h"
#include "uninet/transport.h"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace uninet {

class Node {
public:
    // Delivered to the application after framing/echo/dst filtering.
    using DataHandler = std::function<void(const Envelope& env)>;

    Node(std::string name, Transport* transport, Compression compress = DEFAULT_COMPRESSION);

    const std::string& uuid() const { return uuid_; }
    const std::string& name() const { return name_; }
    Transport* transport() const { return transport_; }

    bool connect();
    bool connected() const;

    // Retry connection with exponential backoff. attempts<=0 means "until connected".
    // Loopback always succeeds on the first try; NatsTransport applies real backoff.
    bool retry_connect(int attempts, double base_sleep_s = 0.1);

    // Publish an arbitrary message payload. dst_uuid="" broadcasts; otherwise the
    // frame is targeted (only the peer whose uuid == dst_uuid accepts it).
    void publish(const std::string& subject, Cbor data, const std::string& dst_uuid = "");

    // Subscribe to a subject (exact or wildcard). The handler receives accepted
    // envelopes only (own echoes and non-matching dst_uuids are filtered).
    void subscribe(const std::string& subject, DataHandler handler);

private:
    std::string name_;
    std::string uuid_;
    Transport* transport_;
    Compression compress_;

    std::mutex handlers_mu_;
    std::vector<std::pair<std::string, DataHandler>> handlers_;

    void on_raw_(const std::string& subject, const Bytes& payload);
};

// Build a UUID: "<name>_<YYYYmmdd-HHMMSS>_<rand>". Lighter-collision than the
// 1-in-1000 random the Slicer peer uses today.
std::string make_uuid(const std::string& name);

}  // namespace uninet
