// UniNet: transport abstraction. A Transport moves framed payloads between
// subjects; the codec and protocol layers above are independent of how the bytes
// travel.
//
// Two implementations ship: ZyreTransport (brokerless peer-to-peer over the
// network, with discovery, what applications use) and LoopbackTransport
// (in-process and deterministic, for tests). A new backend is an addition here,
// not a change anywhere above.
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

    // Publish `len` bytes on `subject` to everyone. Returns false if not
    // connected / failed.
    virtual bool publish(const std::string& subject, const uint8_t* data, size_t len) = 0;

    // Publish to ONE peer. A transport that can address a single peer overrides
    // this and delivers only there; the default returns false and the caller
    // falls back to a broadcast that receivers filter on dst_uuid. ZRE can, so
    // an addressed message really is sent to one device instead of to all of
    // them, which matters when the payload is a 450 KiB mesh.
    virtual bool publish_to(const std::string& peer_uuid, const std::string& subject,
                            const uint8_t* data, size_t len) {
        (void)peer_uuid; (void)subject; (void)data; (void)len; return false;
    }

    // True when publish_to() can really address a single peer. Callers use it
    // to decide whether a failed unicast should fall back to a broadcast: on a
    // transport that cannot address anyone, the fallback is the only delivery
    // path; on one that can, a failure means the peer is gone and broadcasting
    // a payload to everyone for nobody is worse than reporting the failure.
    virtual bool can_address() const { return false; }

    // Subscribe to `subject` (exact match or a wildcard, see below).
    virtual void subscribe(const std::string& subject, MessageHandler handler) = 0;
    virtual void unsubscribe(const std::string& subject) = 0;

    virtual std::string name() const = 0;   // "zyre" / "loopback" / ...
};

// Subject matching:
//   "domain.D1"   exact
//   "domain.>"    matches "domain.D1", "domain.D2.feed" (">" = one-or-more tokens)
//   ">"           matches everything
bool subject_matches(const std::string& pattern, const std::string& subject);

}  // namespace uninet
