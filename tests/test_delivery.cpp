// UniNet: the delivery thread. Proves the one property the network layer is
// built around and that nothing else in the suite covers -- a subscription
// handler that takes its time does not cost the receiver messages.
//
// WHY THIS TEST EXISTS. Handlers used to run on the thread that reads Zyre's
// event outbox. Anything that blocked in a handler blocked the reader, the pipe
// behind it filled, and messages were discarded before UniNet ever saw them.
// The symptom in the field was "we lose packets while the viewer is busy", with
// both ends reporting a healthy connection and nothing in either log. That is
// exactly the failure the tests could not see, because every other test in this
// suite has handlers that return instantly.
//
// The tests below are timing-tolerant about *when* messages arrive (delivery is
// asynchronous by design now) and strict about *whether* they do.
#include "uninet/session.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
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
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// A realm nobody else on this machine shares, so a developer running the demo
// beside the tests cannot perturb them.
std::string unique_realm(const char* tag) {
#ifndef _WIN32
    return std::string("uninet-test-") + tag + "-" + std::to_string(getpid());
#else
    return std::string("uninet-test-") + tag;
#endif
}

// One numbered message. Cbor::set() returns a reference, so chaining off
// `Cbor::map()` directly would pass a reference to a temporary; build it in a
// named local instead.
uninet::Cbor numbered(int n) {
    uninet::Cbor c = uninet::Cbor::map();
    c.set("n", uninet::Cbor::uint(uint64_t(n)));
    return c;
}

// Stops every session it holds when it goes out of scope. Handlers capture
// state by reference, and locals die in reverse declaration order, so this is
// declared AFTER everything a handler touches: it is then destroyed first and
// the sessions are quiet before that state goes away.
struct StopFirst {
    std::vector<uninet::Session*> sessions;
    ~StopFirst() { for (auto* s : sessions) s->close(); }
};

// ── the property this all exists for ──────────────────────────────────────
// A handler that blocks for far longer than the gap between messages must not
// cost a single message. Before the delivery thread, the sender outran the
// receiver's reader and the surplus was dropped inside ZeroMQ.
void test_slow_handler_loses_nothing() {
    std::printf("slow handler loses nothing\n");

    constexpr int kMessages = 40;
    //: Long enough that the whole run (40 x 25 ms = 1 s) cannot finish inside
    //: the burst, so the queue is genuinely used, and short enough that the
    //: test stays quick.
    constexpr auto kHandlerCost = std::chrono::milliseconds(25);

    std::mutex mu;
    std::vector<int> got;

    uninet::SessionConfig cfg;
    cfg.realm = unique_realm("slow");
    auto tx = uninet::Session::join("sender", cfg);
    auto rx = uninet::Session::join("receiver", cfg);
    StopFirst stop{{tx.get(), rx.get()}};

    rx->subscribe("t.>", [&](const uninet::Envelope& e) {
        // The point of the test: a handler that is slow on purpose.
        std::this_thread::sleep_for(kHandlerCost);
        std::lock_guard<std::mutex> lk(mu);
        got.push_back(int(e.data["n"].as_uint()));
    });

    if (!wait_until([&] { return !tx->peers().empty() && !rx->peers().empty(); },
                    std::chrono::seconds(10))) {
        check(false, "the two nodes found each other");
        return;
    }

    // Send far faster than the handler can consume. Every one of these has to
    // survive in the queue.
    const auto send_began = std::chrono::steady_clock::now();
    for (int n = 0; n < kMessages; ++n)
        tx->publish("t.x", numbered(n));
    const auto send_took = std::chrono::steady_clock::now() - send_began;

    // Publishing must not have waited for the receiver's handlers: the sender
    // hands frames to its own network thread and returns. If this ever starts
    // taking about kMessages * kHandlerCost, delivery has been coupled back to
    // the network thread somewhere.
    check(send_took < kMessages * kHandlerCost / 2,
          "publishing does not block on the receiver's handlers");

    const bool all = wait_until([&] {
        std::lock_guard<std::mutex> lk(mu);
        return got.size() == size_t(kMessages);
    }, std::chrono::seconds(30));

    std::lock_guard<std::mutex> lk(mu);
    check(all, "every message reached a handler that took " +
               std::to_string(kHandlerCost.count()) + " ms each");
    if (!all) std::printf("       (got %zu of %d)\n", got.size(), kMessages);

    // Order is part of the contract: one queue, one thread, arrival order.
    bool ordered = true;
    for (size_t i = 0; i < got.size(); ++i)
        if (got[i] != int(i)) ordered = false;
    check(ordered, "they arrived in the order they were sent");

    const auto stats = rx->transport().delivery_stats();
    check(stats.threaded, "handlers ran off the network thread");
    check(stats.dropped == 0, "nothing was dropped");
    check(stats.peak_queued > 1,
          "the queue actually absorbed the burst (peak " +
          std::to_string(stats.peak_queued) + ")");
    check(stats.slowest_handler_us >= 20000,
          "the slow handler is visible in the stats, so it can be found");
}

// ── the network thread keeps reading while a handler is stuck ─────────────
// Not the same claim as above. Here the handler blocks for longer than any
// reasonable burst, and we assert that presence and peer bookkeeping -- which
// the network thread owns -- carry on regardless.
void test_network_thread_stays_live() {
    std::printf("\nthe network keeps running while a handler blocks\n");

    std::atomic<bool> release{false};
    std::atomic<int>  entered{0};

    uninet::SessionConfig cfg;
    cfg.realm = unique_realm("live");
    auto tx = uninet::Session::join("sender", cfg);
    auto rx = uninet::Session::join("receiver", cfg);
    StopFirst stop{{tx.get(), rx.get()}};

    rx->subscribe("t.>", [&](const uninet::Envelope&) {
        entered.fetch_add(1);
        // Hold the delivery thread hostage until the test lets go.
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });

    if (!wait_until([&] { return !rx->peers().empty(); }, std::chrono::seconds(10))) {
        check(false, "the two nodes found each other");
        release.store(true);
        return;
    }

    tx->publish("t.x", uninet::Cbor::map());
    check(wait_until([&] { return entered.load() == 1; }, std::chrono::seconds(5)),
          "the handler is running and is not coming back");

    // With the handler wedged, a third node joins. The receiver's network
    // thread has to notice it. On the old code it could not: it was inside the
    // handler.
    //
    // NOT registered with `stop`: it is declared after it, so it is destroyed
    // BEFORE it, and `stop` would then close a Session that no longer exists.
    // Its own destructor closes it, which is all that is needed here.
    auto other = uninet::Session::join("latecomer", cfg);
    check(wait_until([&] { return rx->peers().size() == 2; }, std::chrono::seconds(10)),
          "a new device is still discovered while a handler is blocked");

    // And further messages are still taken off the wire: they queue rather
    // than being lost, which delivery_stats() can prove while it is happening.
    for (int n = 0; n < 5; ++n) tx->publish("t.x", uninet::Cbor::map());
    check(wait_until([&] { return rx->transport().delivery_stats().queued >= 5; },
                     std::chrono::seconds(10)),
          "messages sent meanwhile are queued, not discarded");

    release.store(true);
    check(wait_until([&] { return entered.load() == 6; }, std::chrono::seconds(20)),
          "and they are all delivered once the handler returns");
}

// ── a slow-but-progressing consumer loses nothing ─────────────────────────
// The other half of the cap policy, and the one a Blob transfer depends on.
// When the queue fills against a consumer that is still draining, the network
// thread WAITS rather than discarding: the pressure goes back down the TCP
// connection to the sender, and the transfer stays intact. Dropping here is
// what corrupts a multi-gigabyte transfer into "chunk out of order".
void test_backpressure_beats_dropping() {
    std::printf("\na slow consumer gets backpressure, not lost messages\n");

    constexpr int kMessages = 30;
    std::mutex mu;
    std::vector<int> got;

    uninet::SessionConfig cfg;
    cfg.realm = unique_realm("press");
    //: A queue with room for two, against thirty messages: the cap is hit
    //: almost immediately and stays hit for the whole run.
    cfg.max_delivery_messages = 2;
    //: Long enough to cover the whole backlog (30 x 10 ms), so the wait never
    //: expires and nothing is ever dropped.
    cfg.delivery_block_ms = 10000;
    auto tx = uninet::Session::join("sender", cfg);
    auto rx = uninet::Session::join("receiver", cfg);
    StopFirst stop{{tx.get(), rx.get()}};

    rx->subscribe("t.>", [&](const uninet::Envelope& e) {
        // Slower than the sender, but always progressing. This is a loaded
        // consumer, not a stalled one, and the difference is the whole point.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> lk(mu);
        got.push_back(int(e.data["n"].as_uint()));
    });

    if (!wait_until([&] { return !rx->peers().empty(); }, std::chrono::seconds(10))) {
        check(false, "the two nodes found each other");
        return;
    }

    for (int n = 0; n < kMessages; ++n) tx->publish("t.x", numbered(n));

    const bool all = wait_until([&] {
        std::lock_guard<std::mutex> lk(mu);
        return got.size() == size_t(kMessages);
    }, std::chrono::seconds(30));

    const auto stats = rx->transport().delivery_stats();
    std::lock_guard<std::mutex> lk(mu);
    check(all, "every message survived a queue two deep");
    if (!all) std::printf("       (got %zu of %d)\n", got.size(), kMessages);
    check(stats.dropped == 0, "nothing was dropped: the cap applied pressure instead");
    check(stats.blocked_us > 0,
          "and the wait is recorded, so a loaded transfer is distinguishable "
          "from a stalled one");

    bool ordered = true;
    for (size_t i = 0; i < got.size(); ++i)
        if (got[i] != int(i)) ordered = false;
    check(ordered, "in order");
}

// ── a consumer that has STOPPED must not wedge the node ───────────────────
// The other side of the same policy. Waiting forever for a handler that is
// never coming back would take discovery, presence and every other subject
// down with it. So the wait is bounded, and past it UniNet drops the OLDEST
// messages, counts every one, and keeps delivering the newest -- the right end
// to keep for the live state this library carries.
void test_cap_drops_oldest_and_counts() {
    std::printf("\na stalled consumer costs a bounded wait, then the oldest messages\n");

    std::atomic<bool> release{false};
    std::atomic<int>  entered{0};
    std::mutex mu;
    std::vector<int> got;

    uninet::SessionConfig cfg;
    cfg.realm = unique_realm("cap");
    //: Room for a handful of messages, so the cap is reached in a burst this
    //: test can send in a few milliseconds.
    cfg.max_delivery_messages = 4;
    //: Far shorter than the 5 s default, so the test does not sit through the
    //: production budget to prove the behaviour past it.
    cfg.delivery_block_ms = 200;
    auto tx = uninet::Session::join("sender", cfg);

    uninet::SessionConfig rxcfg = cfg;
    auto rx = uninet::Session::join("receiver", rxcfg);
    StopFirst stop{{tx.get(), rx.get()}};

    rx->subscribe("t.>", [&](const uninet::Envelope& e) {
        if (entered.fetch_add(1) == 0)
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::lock_guard<std::mutex> lk(mu);
        got.push_back(int(e.data["n"].as_uint()));
    });

    if (!wait_until([&] { return !rx->peers().empty(); }, std::chrono::seconds(10))) {
        check(false, "the two nodes found each other");
        release.store(true);
        return;
    }

    // The first message wedges the handler; the next 20 pile up behind a queue
    // that holds 4.
    for (int n = 0; n < 21; ++n)
        tx->publish("t.x", numbered(n));

    check(wait_until([&] { return rx->transport().delivery_stats().dropped > 0; },
                     std::chrono::seconds(10)),
          "the overflow is reported rather than hidden");

    release.store(true);
    // Let the backlog drain.
    wait_until([&] { return rx->transport().delivery_stats().queued == 0; },
               std::chrono::seconds(10));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto stats = rx->transport().delivery_stats();
    std::lock_guard<std::mutex> lk(mu);
    check(got.size() < 21, "some messages were genuinely dropped");
    check(stats.dropped + stats.delivered >= got.size(),
          "delivered + dropped accounts for what arrived");
    // The newest is what a live application wants. Whatever else was lost, the
    // last message sent must be one of the ones kept.
    check(!got.empty() && got.back() == 20,
          "the most recent message survived; the stale ones are what went");
}

// ── the escape hatch still works ──────────────────────────────────────────
// Running handlers on the network thread is still available for a caller who
// wants the shorter path and knows their handlers are trivial. It has to be
// exactly as correct, just without the decoupling.
void test_inline_delivery_still_works() {
    std::printf("\nhandlers on the network thread (the escape hatch)\n");

    std::mutex mu;
    std::vector<int> got;

    uninet::SessionConfig cfg;
    cfg.realm = unique_realm("inline");
    cfg.deliver_on_network_thread = true;
    auto tx = uninet::Session::join("sender", cfg);
    auto rx = uninet::Session::join("receiver", cfg);
    StopFirst stop{{tx.get(), rx.get()}};

    rx->subscribe("t.>", [&](const uninet::Envelope& e) {
        std::lock_guard<std::mutex> lk(mu);
        got.push_back(int(e.data["n"].as_uint()));
    });

    if (!wait_until([&] { return !rx->peers().empty(); }, std::chrono::seconds(10))) {
        check(false, "the two nodes found each other");
        return;
    }

    for (int n = 0; n < 10; ++n)
        tx->publish("t.x", numbered(n));

    const bool all = wait_until([&] {
        std::lock_guard<std::mutex> lk(mu);
        return got.size() == 10;
    }, std::chrono::seconds(15));
    check(all, "messages are delivered with no delivery thread");
    check(!rx->transport().delivery_stats().threaded,
          "and the stats say which mode this is, so a report can be read");
}

// ── closing while the queue is not empty ──────────────────────────────────
// close() drains before it stops. A message that had already reached this
// process is one the application was going to see, and an orderly shutdown
// must not lose data that a crash would not.
void test_close_drains() {
    std::printf("\nclose() delivers what it is already holding\n");

    std::atomic<bool> release{false};
    std::atomic<int>  delivered{0};

    uninet::SessionConfig cfg;
    cfg.realm = unique_realm("drain");
    auto tx = uninet::Session::join("sender", cfg);
    auto rx = uninet::Session::join("receiver", cfg);
    StopFirst stop{{tx.get()}};       // rx is closed by hand below

    rx->subscribe("t.>", [&](const uninet::Envelope&) {
        if (delivered.fetch_add(1) == 0)
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    });

    if (!wait_until([&] { return !rx->peers().empty(); }, std::chrono::seconds(10))) {
        check(false, "the two nodes found each other");
        release.store(true);
        rx->close();
        return;
    }

    for (int n = 0; n < 6; ++n) tx->publish("t.x", uninet::Cbor::map());
    // Wait until they are demonstrably in the queue behind the wedged handler.
    wait_until([&] { return rx->transport().delivery_stats().queued >= 5; },
               std::chrono::seconds(10));

    // Close from another thread, so the wedged handler can be released while
    // close() is already waiting on it. This is the shutdown-ordering case
    // that a naive "stop the thread, then drop the queue" would get wrong.
    std::thread closer([&] { rx->close(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    release.store(true);
    closer.join();

    check(delivered.load() == 6,
          "everything already received was delivered before close() returned");
    if (delivered.load() != 6)
        std::printf("       (delivered %d of 6)\n", delivered.load());
}

}  // namespace

int main() {
    std::printf("UniNet delivery tests: %s\n\n", uninet::zyre_version_string().c_str());

    test_slow_handler_loses_nothing();
    test_network_thread_stays_live();
    test_backpressure_beats_dropping();
    test_cap_drops_oldest_and_counts();
    test_inline_delivery_still_works();
    test_close_drains();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
