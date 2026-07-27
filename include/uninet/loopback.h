// UniNet: in-process transport. Routes publishes to matching subscribers (exact
// subject or ">" wildcard) on the SAME LoopbackTransport instance, so
// two Nodes sharing one LoopbackTransport talk to each other. Synchronous dispatch
//: deterministic for tests/benchmarks. The always-available backend (no broker,
// no network), analogous to UniVox's CPU baseline. An async thread-pool dispatch
// mode is staged.
#pragma once

#include "uninet/transport.h"

#include <atomic>
#include <mutex>
#include <vector>

namespace uninet {

class LoopbackTransport : public Transport {
public:
    // online_/delivered_ are touched from whatever thread calls connect(),
    // publish() or delivered(): ThreadSanitizer flagged both as races when they
    // were a plain bool and a plain counter (the counter was incremented under
    // mu_ but read without it). Atomics, not the mutex: publish() reads the flag
    // before it takes any lock.
    bool connect() override { online_.store(true, std::memory_order_relaxed); return true; }
    void disconnect() override { online_.store(false, std::memory_order_relaxed); }
    bool connected() const override { return online_.load(std::memory_order_relaxed); }

    bool publish(const std::string& subject, const uint8_t* data, size_t len) override;
    void subscribe(const std::string& subject, MessageHandler handler) override;
    void unsubscribe(const std::string& subject) override;
    std::string name() const override { return "loopback"; }

    // Count of delivered messages since creation (for the benchmark/tests).
    uint64_t delivered() const { return delivered_.load(std::memory_order_relaxed); }

private:
    struct Sub {
        std::string pattern;
        MessageHandler handler;
    };
    mutable std::mutex mu_;
    std::vector<Sub> subs_;
    std::atomic<bool> online_{false};
    std::atomic<uint64_t> delivered_{0};
};

}  // namespace uninet
