// UniNet: in-process transport implementation.
#include "uninet/loopback.h"
#include "uninet/profiler.h"

#include <algorithm>   // std::remove_if (unsubscribe)
#include <deque>

namespace uninet {

bool LoopbackTransport::publish(const std::string& subject, const uint8_t* data, size_t len) {
    if (!online_.load(std::memory_order_relaxed)) return false;

    // The payload must be copied: `data` normally points at the publisher's
    // reusable framing buffer, so a handler that publishes re-entrantly would
    // otherwise clobber the bytes still being delivered. Allocating that copy
    // fresh each time costs an mmap/munmap pair per message once frames pass
    // glibc's ~128 KiB threshold, so keep one buffer per re-entrancy depth and
    // reuse it: depth 0 is the steady state and allocates nothing after the
    // first message.
    // A deque, because a re-entrant publish grows the pool while an outer frame
    // still holds a reference into it: deque keeps existing elements pinned.
    static thread_local std::deque<Bytes> pool;
    static thread_local size_t depth = 0;
    while (depth >= pool.size()) pool.emplace_back();
    Bytes& payload = pool[depth];
    payload.assign(data, data + len);

    struct DepthGuard {   // restore the depth even if a handler throws
        size_t& d;
        ~DepthGuard() { --d; }
    };

    // Snapshot matching handlers under the lock; deliver outside it so a handler
    // may publish/subscribe without deadlocking.
    std::vector<MessageHandler> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : subs_)
            if (subject_matches(s.pattern, subject))
                targets.push_back(s.handler);
        delivered_.fetch_add(1, std::memory_order_relaxed);
    }
    profiler::ScopedOp _("loopback.deliver", len, len);
    ++depth;
    DepthGuard guard{depth};
    for (auto& h : targets) h(subject, payload);
    return true;
}

void LoopbackTransport::subscribe(const std::string& subject, MessageHandler handler) {
    std::lock_guard<std::mutex> lk(mu_);
    subs_.push_back({subject, std::move(handler)});
}

void LoopbackTransport::unsubscribe(const std::string& subject) {
    std::lock_guard<std::mutex> lk(mu_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                               [&](const Sub& s) { return s.pattern == subject; }),
                subs_.end());
}

bool subject_matches(const std::string& pattern, const std::string& subject) {
    if (pattern.empty() || pattern == subject) return pattern == subject;
    // A ">" as the final token matches one-or-more trailing tokens.
    if (pattern == ">") return true;
    if (pattern.size() >= 2 && pattern.back() == '>' && pattern[pattern.size() - 2] == '.') {
        std::string prefix = pattern.substr(0, pattern.size() - 1);  // keep trailing "."
        if (subject.compare(0, prefix.size(), prefix) == 0 && subject.size() > prefix.size())
            return true;
        return false;
    }
    return pattern == subject;
}

}  // namespace uninet
