// UniNet — transport abstraction. A Transport moves framed payloads between
// subjects; the protocol/codec layers above are independent of how bytes travel.
// v0.1 ships LoopbackTransport (in-process, always available) and an optional
// NatsTransport (the production brokered backend). Mesh / BLE / in-proc-discovery
// backends are future implementations of this same interface — the layers above
// never change when a transport is added.
#pragma once

#include "uninet/types.h"

#include <functional>
#include <string>

namespace uninet {

// Delivered to a subscriber: the subject the message arrived on + the framed
// payload (the Node above unfames it into an Envelope).
using MessageHandler = std::function<void(const std::string& subject, const Bytes& payload)>;

class Transport {
public:
    virtual ~Transport() = default;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool connected() const = 0;

    // Publish `len` bytes on `subject`. Returns false if not connected / failed.
    virtual bool publish(const std::string& subject, const uint8_t* data, size_t len) = 0;
    // Subscribe to `subject` (exact match or a NATS-style wildcard, see below).
    virtual void subscribe(const std::string& subject, MessageHandler handler) = 0;
    virtual void unsubscribe(const std::string& subject) = 0;

    virtual std::string name() const = 0;   // "loopback" / "nats" / ...
};

// NATS-style subject matching:
//   "domain.D1"   exact
//   "domain.>"    matches "domain.D1", "domain.D2.feed" (">" = one-or-more tokens)
//   ">"           matches everything
bool subject_matches(const std::string& pattern, const std::string& subject);

}  // namespace uninet
