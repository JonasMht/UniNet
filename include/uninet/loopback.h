// UniNet — in-process transport. Routes publishes to matching subscribers (exact
// subject or NATS-style ">" wildcard) on the SAME LoopbackTransport instance, so
// two Nodes sharing one LoopbackTransport talk to each other. Synchronous dispatch
// — deterministic for tests/benchmarks. The always-available backend (no broker,
// no network), analogous to UniVox's CPU baseline. An async thread-pool dispatch
// mode is staged.
#pragma once

#include "uninet/transport.h"

#include <mutex>
#include <vector>

namespace uninet {

class LoopbackTransport : public Transport {
public:
    bool connect() override { online_ = true; return true; }
    void disconnect() override { online_ = false; }
    bool connected() const override { return online_; }

    bool publish(const std::string& subject, const uint8_t* data, size_t len) override;
    void subscribe(const std::string& subject, MessageHandler handler) override;
    void unsubscribe(const std::string& subject) override;
    std::string name() const override { return "loopback"; }

    // Count of delivered messages since creation (for the benchmark/tests).
    uint64_t delivered() const { return delivered_; }

private:
    struct Sub {
        std::string pattern;
        MessageHandler handler;
    };
    mutable std::mutex mu_;
    std::vector<Sub> subs_;
    bool online_ = false;
    uint64_t delivered_ = 0;
};

}  // namespace uninet
