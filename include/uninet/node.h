// UniNet: high-level peer. A Node owns a UUID, a Transport, and a default
// compression. It encodes Envelopes, suppresses self-echo, honors dst_uuid
// targeting, and reconnects with backoff. This is the single place the protocol
// lives, so no application has to reimplement it.
#pragma once

#include "uninet/codec.h"
#include "uninet/transport.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
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

    std::string uuid() const;
    const std::string& name() const { return name_; }

    // Adopt a new identity, after the transport rebuilt itself on another
    // network. ZRE mints a fresh uuid every time its node is created, and this
    // one is what outgoing messages carry and what incoming `dst` is matched
    // against: leaving it stale makes every addressed message to this device be
    // dropped, silently, while broadcasts keep working. Session wires this to
    // ZyreTransport::on_reconnected.
    void set_uuid(std::string uuid);
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

    // ── unmatched-message diagnostics and late-subscriber buffer ───────────
    // A message that arrives with no matching subscription is held in a
    // bounded FIFO (see on_raw_) and delivered, in arrival order, when a
    // matching subscription is first registered. Counting and holding it
    // instead of discarding it in silence is what turns "messages only arrive
    // while the UI is open" into a diagnosable application-lifetime bug.
    struct Stats {
        uint64_t received = 0;                 // arrived at dispatch
        uint64_t delivered = 0;                // >=1 matching handler existed at
                                               //   arrival (delivery attempted)
        uint64_t unmatched = 0;                // zero handlers at arrival
        uint64_t buffered_current = 0;         // held for a future subscriber
        size_t   buffered_current_bytes = 0;
        uint64_t buffered_delivered = 0;       // drained to a later subscriber
        uint64_t buffered_dropped = 0;         // evicted before any match
        uint64_t errored = 0;                  // handler threw
    };
    Stats stats() const;
    std::vector<std::string> subscriptions() const;

    // Change the unmatched-message caps or disable the buffer entirely. A cap
    // of 0 means "no limit" for that dimension. When disabled, unmatched
    // messages are discarded (still counted in stats().unmatched, still
    // warned about once per subject). Safe to call any time, from any thread.
    void set_buffer_limits(size_t max_bytes, size_t max_messages, bool enabled);

private:
    std::string name_;
    // Written by a reconnect on the network thread, read by publish() on any
    // caller thread, so it is guarded. The lock is uncontended and costs far
    // less than the framing and compression that follow it.
    mutable std::mutex uuid_mu_;
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

    mutable std::mutex handlers_mu_;
    std::vector<std::pair<std::string, DataHandler>> handlers_;
    // handlers_mu_ is mutable: subscriptions() is const and must read the
    // handler list while a network thread may be dispatching through it.

    // Held for a future subscriber instead of discarded: see on_raw_.
    // Envelopes own refcounted Bytes, so holding them is cheap until a cap
    // is hit.  All of this is guarded by buffer_mu_.
    struct BufferedMessage {
        std::string subject;
        Envelope env;
    };
    mutable std::mutex buffer_mu_;
    std::deque<BufferedMessage> buffer_;
    size_t buffer_bytes_ = 0;
    size_t max_buffer_bytes_ = 8u * 1024u * 1024u;  // 8 MiB; 0 = no byte cap
    size_t max_buffer_messages_ = 256;              // 0 = no count cap
    bool buffer_enabled_ = true;
    // Warn once per subject (a flood must not become a log flood), and fail
    // toward warning: if a set ever saturates it is cleared so warnings
    // resume. Guarded by buffer_mu_.
    std::unordered_set<std::string> warned_no_subject_;
    std::unordered_set<std::string> warned_evicted_;
    std::atomic<uint64_t> stat_received_{0};
    std::atomic<uint64_t> stat_delivered_{0};
    std::atomic<uint64_t> stat_unmatched_{0};
    std::atomic<uint64_t> stat_buffered_delivered_{0};
    std::atomic<uint64_t> stat_buffered_dropped_{0};
    std::atomic<uint64_t> stat_errored_{0};

    void ensure_watching_();   // create the internal ">" subscription once the transport is up
    void on_raw_(const std::string& subject, const Bytes& payload);
    void buffer_unmatched_(const std::string& subject, const Envelope& env);
    void drain_buffer_(const std::string& pattern);
    // Warning helpers; callers hold buffer_mu_.
    void warn_no_subject_(const std::string& subject, bool buffering);
    void warn_evicted_(const std::string& subject);
};

// Build a UUID: "<name>_<YYYYmmdd-HHMMSS>_<rand>". Lighter-collision than the
// 1-in-1000 random the Slicer peer uses today.
std::string make_uuid(const std::string& name);

}  // namespace uninet
