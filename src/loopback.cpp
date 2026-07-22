// UniNet — in-process transport implementation.
#include "uninet/loopback.h"

namespace uninet {

bool LoopbackTransport::publish(const std::string& subject, const uint8_t* data, size_t len) {
    if (!online_) return false;
    // Copy the payload so handlers can publish re-entrantly without aliasing.
    Bytes payload(data, data + len);
    // Snapshot matching handlers under the lock; deliver outside it so a handler
    // may publish/subscribe without deadlocking.
    std::vector<MessageHandler> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : subs_)
            if (subject_matches(s.pattern, subject))
                targets.push_back(s.handler);
        ++delivered_;
    }
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
    // NATS-style ">" wildcard as the final token matches one-or-more trailing tokens.
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
