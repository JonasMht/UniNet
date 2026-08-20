// UniNet: unmatched-message buffer and delivery diagnostics.
//
// Regression test for the class of bug where an application registers its
// subscription later than the first relevant message -- a UI-gated module, a
// lazy initializer -- and messages are lost in silence. The transport hears
// everything, so the loss was always an application-lifetime problem; this
// suite locks in the contract that makes it diagnosable and recoverable:
//
//  1. messages with zero matching subscriptions are COUNTED (stats().unmatched);
//  2. they are HELD in a bounded FIFO, not discarded;
//  3. the first matching subscription receives them, in arrival order,
//     exactly once (stats().buffered_delivered);
//  4. caps evict oldest-first (stats().buffered_dropped), never unbounded;
//  5. disabling the buffer restores drop-in-silence, still counted.
//
// Same harness as test_network.cpp: two real nodes, real discovery, tolerant
// about timing, strict about outcomes.
#include "uninet/session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
  #include <unistd.h>      // getpid, for a realm nobody else on this box shares
#endif

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("  %s %s\n", cond ? "ok  " : "FAIL", what.c_str());
    if (!cond) ++g_failures;
}

template <typename F>
bool wait_until(F pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return pred();
}

struct StopFirst {
    std::vector<uninet::Session*> sessions;
    explicit StopFirst(std::initializer_list<uninet::Session*> s) : sessions(s) {}
    ~StopFirst() { for (auto* s : sessions) if (s) s->close(); }
    StopFirst(const StopFirst&) = delete;
    StopFirst& operator=(const StopFirst&) = delete;
};

std::string unique_realm(const char* tag) {
    static std::atomic<unsigned> n{0};
    return std::string("uninet-test-") + tag + "-" +
           std::to_string(
#ifdef _WIN32
               0
#else
               ::getpid()
#endif
           ) + "-" + std::to_string(n.fetch_add(1));
}

// ── unmatched messages are counted, held, then delivered in order ─────────
void test_unmatched_buffer() {
    std::printf("unmatched-buffer\n");
    const std::string realm = unique_realm("unmatched");
    uninet::SessionConfig cfg; cfg.realm = realm;

    auto a = uninet::Session::join("LateBinder", cfg);
    auto b = uninet::Session::join("EarlyTalker", cfg);
    StopFirst stop{a.get(), b.get()};

    check(wait_until([&] { return a->peers().size() == 1 && b->peers().size() == 1; },
                     std::chrono::seconds(30)),
          "the two nodes discovered each other");

    // A has NOT subscribed. B talks: every message is unmatched.
    const int pre = 8;
    for (int i = 0; i < pre; ++i)
        b->publish("late." + std::to_string(i), uninet::Cbor::uint(i));

    check(wait_until([&] { return a->stats().unmatched == uint64_t(pre); },
                     std::chrono::seconds(30)),
          "all pre-subscription messages were counted as unmatched");
    {
        const auto st = a->stats();
        check(st.received == uint64_t(pre), "received == pre-subscription count");
        check(st.delivered == 0, "nothing delivered yet (nobody subscribed)");
        check(st.buffered_current == uint64_t(pre), "all unmatched messages held in the buffer");
        check(st.buffered_delivered == 0, "none drained yet");
    }
    check(a->subscriptions().empty(), "subscriptions() reports none before any subscribe");

    // The missing subscriber shows up: buffered messages must drain in order.
    std::mutex mu;
    std::vector<uint64_t> seen;
    a->subscribe("late.>", [&](const uninet::Envelope& e) {
        std::lock_guard<std::mutex> lk(mu);
        seen.push_back(e.data.as_uint());
    });

    check(wait_until([&] { return a->stats().buffered_delivered == uint64_t(pre); },
                     std::chrono::seconds(30)),
          "the first matching subscription drained the whole buffer");
    {
        std::lock_guard<std::mutex> lk(mu);
        check(seen.size() == size_t(pre), "the handler received every buffered message");
        bool ordered = true;
        for (int i = 0; i < pre; ++i)
            if (seen[size_t(i)] != uint64_t(i)) { ordered = false; break; }
        check(ordered, "buffered messages were delivered in arrival order");
    }
    {
        const auto st = a->stats();
        check(st.buffered_current == 0, "buffer empty after the drain");
        check(st.delivered == 0, "still nothing delivered at arrival time (all were buffered)");
    }
    {
        const auto subs = a->subscriptions();
        check(subs.size() == 1 && subs[0] == "late.>", "subscriptions() now lists the pattern");
    }

    // Live traffic after the subscription is delivered normally, not double-
    // counted against the buffer.
    for (int i = 0; i < 3; ++i)
        b->publish("late." + std::to_string(pre + i), uninet::Cbor::uint(pre + i));
    check(wait_until([&] { return a->stats().delivered == 3; }, std::chrono::seconds(30)),
          "post-subscription messages delivered live");
    check(wait_until([&] { return a->stats().unmatched == uint64_t(pre); },
                     std::chrono::seconds(10)),
          "live messages did not add to unmatched");

    // Bounded: tight caps evict oldest-first instead of growing.
    a->set_buffer_limits(0, 2, true);
    for (int i = 0; i < 5; ++i)
        b->publish("evict." + std::to_string(i), uninet::Cbor::uint(i));
    check(wait_until([&] { return a->stats().unmatched == uint64_t(pre + 5); },
                     std::chrono::seconds(30)),
          "eviction-phase messages counted as unmatched");
    check(wait_until([&] { return a->stats().buffered_dropped == 3; },
                     std::chrono::seconds(10)),
          "three were evicted once the cap was hit");
    {
        const auto st = a->stats();
        check(st.buffered_current == 2, "buffer held to the cap (2 messages)");
    }

    // Disabling drains what is left (counted as dropped) and discards
    // thereafter, still counted as unmatched.
    a->set_buffer_limits(0, 0, false);
    check(wait_until([&] { return a->stats().buffered_dropped == 5; },
                     std::chrono::seconds(10)),
          "disabling evicted the remaining buffered messages");
    b->publish("deny.x", uninet::Cbor::uint(1));
    check(wait_until([&] { return a->stats().unmatched == uint64_t(pre + 5 + 1); },
                     std::chrono::seconds(30)),
          "messages while disabled still counted as unmatched");
    check(a->stats().buffered_current == 0, "buffer stays empty while disabled");

    // Handler exceptions are counted and isolated (a throwing subscriber must
    // not stop delivery for anything else).
    a->subscribe("panic.>", [](const uninet::Envelope&) {
        throw std::runtime_error("intentional test throw");
    });
    b->publish("panic.a", uninet::Cbor::uint(1));
    b->publish("panic.b", uninet::Cbor::uint(2));
    check(wait_until([&] { return a->stats().errored == 2; }, std::chrono::seconds(30)),
          "throwing handlers are counted as errored, per message");
    check(wait_until([&] { return a->stats().delivered == 3 + 2; },
                     std::chrono::seconds(10)),
          "delivered counts at-arrival deliveries only (buffered drain separate)");
    {
        const auto st = a->stats();
        check(st.delivered == 5 && st.buffered_delivered == uint64_t(pre) && st.errored == 2,
              "final counters consistent: 5 live, 8 drained, 2 errored");
    }
}

}  // namespace

int main() {
    test_unmatched_buffer();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}