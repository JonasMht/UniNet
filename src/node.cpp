// UniNet: Node implementation (the high-level peer).
#include "uninet/node.h"
#include "uninet/profiler.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <thread>

namespace uninet {

// gmtime is not thread-safe, and the reentrant spelling differs per platform:
// POSIX has gmtime_r, MSVC has gmtime_s with the arguments the other way round.
// This was the single POSIX-only call in the tree, on the constructor path of
// every Node. It alone made an MSVC build impossible.
static bool gmtime_utc(std::time_t t, std::tm& out) {
#ifdef _WIN32
    return gmtime_s(&out, &t) == 0;
#else
    return gmtime_r(&t, &out) != nullptr;
#endif
}

std::string make_uuid(const std::string& name) {
    using clock = std::chrono::system_clock;
    std::time_t now = clock::to_time_t(clock::now());
    std::tm tm{};
    if (!gmtime_utc(now, tm)) tm = std::tm{};   // a bad clock must not lose the uuid
    char ts[24];
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    static std::atomic<unsigned> seq{0};
    static thread_local std::mt19937 rng{std::random_device{}()};
    unsigned r = rng();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s_%s_%08x%04x", name.c_str(), ts, r, seq.fetch_add(1) & 0xFFFF);
    return buf;
}

Node::Node(std::string name, Transport* transport, Compression compress)
    : name_(std::move(name)), uuid_(make_uuid(name_)), transport_(transport), compress_(compress) {
    ensure_watching_();   // covers the common pattern where the transport was connected first
}

Node::Node(std::string name, std::string uuid, Transport* transport, Compression compress)
    : name_(std::move(name)),
      uuid_(uuid.empty() ? make_uuid(name_) : std::move(uuid)),
      transport_(transport),
      compress_(compress) {
    ensure_watching_();
}

void Node::ensure_watching_() {
    // Cheap check first, then re-check under the lock: two callers must not both
    // pass the test and install the subscription.
    if (watching_.load(std::memory_order_acquire)) return;
    if (!transport_ || !transport_->connected()) return;

    std::lock_guard<std::mutex> lk(watch_mu_);
    if (watching_.load(std::memory_order_relaxed)) return;
    transport_->subscribe(">", [this](const std::string& subject, const Bytes& payload) {
        this->on_raw_(subject, payload);
    });
    watching_.store(true, std::memory_order_release);
}

bool Node::connect() {
    if (!transport_) return false;
    if (transport_->connect() && transport_->connected()) {
        ensure_watching_();
        return true;
    }
    return false;
}

bool Node::connected() const {
    return transport_ && transport_->connected();
}

bool Node::retry_connect(int attempts, double base_sleep_s) {
    if (!transport_) return false;
    if (transport_->connected()) return true;
    int n = (attempts <= 0) ? INT_MAX : attempts;
    double sleep = base_sleep_s;
    for (int i = 0; i < n; ++i) {
        if (transport_->connect() && transport_->connected()) return true;
        if (i + 1 < n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(int(sleep * 1000)));
            sleep *= 2.0;
            if (sleep > 5.0) sleep = 5.0;   // cap the backoff
        }
    }
    return false;
}

std::string Node::uuid() const {
    std::lock_guard<std::mutex> lk(uuid_mu_);
    return uuid_;
}

void Node::set_uuid(std::string uuid) {
    if (uuid.empty()) return;
    std::lock_guard<std::mutex> lk(uuid_mu_);
    uuid_ = std::move(uuid);
}

bool Node::publish(const std::string& subject, Cbor data, const std::string& dst_uuid) {
    if (!transport_ || !transport_->connected()) return false;
    Envelope env;
    env.compression = compress_;
    env.src_uuid = uuid();
    env.dst_uuid = dst_uuid;
    env.subject = subject;
    env.data = std::move(data);
    // Reuse this thread's framing buffers. A fresh Bytes per publish means an
    // mmap/munmap pair for every mesh-sized payload (glibc's mmap threshold
    // starts at 128 KiB); reusing them makes steady-state publishing allocate
    // nothing after the first message.
    static thread_local Scratch scratch;
    static thread_local Bytes wire;
    profiler::ScopedOp _("node.publish");
    frame_into(env, wire, scratch);
    _.set_bytes_in(wire.size());
    _.set_bytes_out(wire.size());
    if (!dst_uuid.empty() && transport_->can_address()) {
        // A transport that can address one peer gets no broadcast fallback. It
        // would send the whole payload to everyone so that nobody accepts it,
        // and still report success, which is how a message to a peer that had
        // just left used to disappear with publish() returning true.
        return transport_->publish_to(dst_uuid, subject, wire.data(), wire.size());
    }
    // Otherwise broadcast: the dst in the clear header still makes every other
    // receiver drop the frame before decoding it.
    return transport_->publish(subject, wire.data(), wire.size());
}

void Node::subscribe(const std::string& subject, DataHandler handler) {
    ensure_watching_();   // app subscribe implies "I want to receive": make sure we're watching
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        handlers_.emplace_back(subject, std::move(handler));
    }
    // A new subscription may match messages that arrived before it existed.
    // They were buffered, not dropped (see on_raw_): deliver them now, in
    // arrival order, on this (the subscribing) thread. Handlers must already
    // tolerate the network thread, so any thread is acceptable.
    drain_buffer_(subject);
}

void Node::on_raw_(const std::string& subject, const Bytes& payload) {
    // Cheap routing filter BEFORE any decompress/decode: read src/dst from the
    // clear header and drop this peer's own echo (and frames not addressed to it)
    // without touching the compressed payload. With LZ4/zlib on, this is what
    // keeps a publisher from decoding every frame it sends.
    auto route = peek_routing(payload);
    if (!route) return;
    // Read once: a reconnect can change it between these two tests, and a
    // message would then be neither our echo nor addressed to us.
    const std::string me = uuid();
    if (route->src == me) return;                          // own echo
    if (!route->dst.empty() && route->dst != me) return;   // not addressed to me

    // A tier this build cannot decode is a configuration problem, not a bad
    // frame, and it is invisible from either end: the sender's publish succeeds,
    // discovery and presence keep working, and only the payloads disappear. Say
    // so once per peer rather than dropping in silence.
    //
    // Found on a Quest-class Android build, where the NDK provides no liblz4:
    // the tablet could send to the workstation and received nothing back, while
    // both sides reported a healthy connection.
    if (!compression_supported(route->compression)) {
        static thread_local std::string warned_for;
        if (warned_for != route->src) {
            warned_for = route->src;
            std::fprintf(stderr,
                         "uninet: dropping messages from %s: they are compressed "
                         "with %s and this build cannot decode that tier. Rebuild "
                         "UniNet with liblz4 available; it is fetched and built "
                         "automatically when the system does not provide it.\n",
                         route->src.c_str(), compression_name(route->compression));
        }
        return;
    }

    // Same reuse on the receive side: unframe_into decodes an uncompressed
    // payload straight out of `payload` (no copy) and decompresses into a
    // buffer that survives across messages.
    static thread_local Scratch scratch;
    auto env = unframe_into(payload.data(), payload.size(), scratch);
    if (!env) return;
    // The docs say readers refuse an unknown major; this used to be an empty
    // statement, so every version was accepted and delivered.
    if (env->protocol_version != CURRENT_PROTOCOL_VERSION) {
        static thread_local uint16_t warned_about = 0;
        if (warned_about != env->protocol_version) {
            warned_about = env->protocol_version;
            std::fprintf(stderr,
                         "uninet: dropping a message with protocol version %u "
                         "(this build speaks %u)\n",
                         unsigned(env->protocol_version),
                         unsigned(CURRENT_PROTOCOL_VERSION));
        }
        return;
    }
    stat_received_.fetch_add(1, std::memory_order_relaxed);
    std::vector<std::pair<std::string, DataHandler>> snapshot;
    bool has_matches = false;
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        for (auto& [pat, h] : handlers_)
            if (subject_matches(pat, subject)) {
                snapshot.emplace_back(pat, h);
                has_matches = true;
            }
    }
    if (!has_matches) {
        // Nobody is subscribed to this yet. The transport hears everything
        // (the internal ">" subscription), so this is never a network loss:
        // it is an application-lifetime problem -- a subscription registered
        // later than the first relevant message, or never. Hold the message
        // for the first matching subscriber instead of discarding it in
        // silence; "messages only arrive while the UI is open" is this line,
        // on a machine where the UI was not.
        stat_unmatched_.fetch_add(1, std::memory_order_relaxed);
        buffer_unmatched_(subject, *env);
        return;
    }
    stat_delivered_.fetch_add(1, std::memory_order_relaxed);
    for (auto& [pat, h] : snapshot) {
        // Per handler, not per message: without this, the first subscriber that
        // threw silently cancelled delivery to every subscriber after it, and
        // the exception was swallowed further up by the transport.
        (void)pat;
        if (!h) continue;
        try {
            h(*env);
        } catch (const std::exception& e) {
            stat_errored_.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "uninet: exception in a subscriber for '%s': %s\n",
                         subject.c_str(), e.what());
        } catch (...) {
            stat_errored_.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "uninet: unknown exception in a subscriber for '%s'\n",
                         subject.c_str());
        }
    }
}

void Node::buffer_unmatched_(const std::string& subject, const Envelope& env) {
    std::lock_guard<std::mutex> lk(buffer_mu_);
    if (!buffer_enabled_) {
        warn_no_subject_(subject, /*buffering=*/false);
        return;
    }
    // Bounded by bytes as well as count: envelopes can carry multi-megabyte
    // blobs, and counting messages alone would let a single blob consume the
    // entire budget. A cap of 0 disables that dimension (see set_buffer_limits).
    const bool count_capped = max_buffer_messages_ > 0;
    const bool bytes_capped = max_buffer_bytes_ > 0;
    while ((count_capped && buffer_.size() >= max_buffer_messages_) ||
           (bytes_capped && buffer_bytes_ >= max_buffer_bytes_)) {
        stat_buffered_dropped_.fetch_add(1, std::memory_order_relaxed);
        buffer_bytes_ -= buffer_.front().env.data.size();
        warn_evicted_(buffer_.front().subject);
        buffer_.pop_front();
    }
    buffer_.emplace_back(BufferedMessage{subject, env});
    buffer_bytes_ += env.data.size();
    warn_no_subject_(subject, /*buffering=*/true);
}

void Node::drain_buffer_(const std::string& pattern) {
    if (!buffer_enabled_) return;
    std::vector<BufferedMessage> matches;
    {
        std::lock_guard<std::mutex> lk(buffer_mu_);
        if (buffer_.empty()) return;
        for (auto it = buffer_.begin(); it != buffer_.end();) {
            if (subject_matches(pattern, it->subject)) {
                buffer_bytes_ -= it->env.data.size();
                matches.push_back(std::move(*it));
                it = buffer_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (matches.empty()) return;
    // Same rules as live delivery, on the subscribing thread. Every handler
    // that matches NOW is newer than the buffered message (any handler
    // present at arrival would have prevented it from being buffered), so
    // every matching handler receives it -- exactly once.
    for (auto& bm : matches) {
        std::vector<std::pair<std::string, DataHandler>> snapshot;
        {
            std::lock_guard<std::mutex> lk(handlers_mu_);
            for (auto& [pat, h] : handlers_)
                if (subject_matches(pat, bm.subject)) snapshot.emplace_back(pat, h);
        }
        if (snapshot.empty()) continue;
        stat_buffered_delivered_.fetch_add(1, std::memory_order_relaxed);
        for (auto& [pat, h] : snapshot) {
            (void)pat;
            if (!h) continue;
            try {
                h(bm.env);
            } catch (const std::exception& e) {
                stat_errored_.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr,
                             "uninet: exception in a buffered subscriber for '%s': %s\n",
                             bm.subject.c_str(), e.what());
            } catch (...) {
                stat_errored_.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr,
                             "uninet: unknown exception in a buffered subscriber for '%s'\n",
                             bm.subject.c_str());
            }
        }
    }
}

void Node::warn_no_subject_(const std::string& subject, bool buffering) {
    // Caller holds buffer_mu_.
    if (warned_no_subject_.count(subject)) return;
    if (warned_no_subject_.size() >= 64) warned_no_subject_.clear();
    warned_no_subject_.insert(subject);
    if (buffering) {
        std::fprintf(stderr,
                     "uninet: '%s' has no matching subscriber; holding it in the "
                     "bounded buffer until one is registered. Register "
                     "subscriptions at application start, not from a UI "
                     "callback. See Session::stats().\n", subject.c_str());
    } else {
        std::fprintf(stderr,
                     "uninet: '%s' has no matching subscriber and the buffer is "
                     "disabled: the message is discarded. Register "
                     "subscriptions at application start, not from a UI "
                     "callback; Session::stats().unmatched counts these.\n",
                     subject.c_str());
    }
}

void Node::warn_evicted_(const std::string& subject) {
    // Caller holds buffer_mu_.
    if (warned_evicted_.count(subject)) return;
    if (warned_evicted_.size() >= 64) warned_evicted_.clear();
    warned_evicted_.insert(subject);
    std::fprintf(stderr,
                 "uninet: unmatched '%s' evicted from the subscriber buffer "
                 "(%zu messages, %zu bytes held): the buffer is bounded, and "
                 "messages that arrive before any matching subscription can "
                 "still be lost. Raise the limits or subscribe earlier; "
                 "Session::stats().buffered_dropped counts these.\n",
                 subject.c_str(), buffer_.size(), buffer_bytes_);
}

Node::Stats Node::stats() const {
    Stats st;
    st.received = stat_received_.load(std::memory_order_relaxed);
    st.delivered = stat_delivered_.load(std::memory_order_relaxed);
    st.unmatched = stat_unmatched_.load(std::memory_order_relaxed);
    st.buffered_delivered = stat_buffered_delivered_.load(std::memory_order_relaxed);
    st.buffered_dropped = stat_buffered_dropped_.load(std::memory_order_relaxed);
    st.errored = stat_errored_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(buffer_mu_);
        st.buffered_current = buffer_.size();
        st.buffered_current_bytes = buffer_bytes_;
    }
    return st;
}

std::vector<std::string> Node::subscriptions() const {
    std::lock_guard<std::mutex> lk(handlers_mu_);
    std::vector<std::string> out;
    out.reserve(handlers_.size());
    for (auto& [pat, h] : handlers_) {
        (void)h;
        out.push_back(pat);
    }
    return out;
}

void Node::set_buffer_limits(size_t max_bytes, size_t max_messages, bool enabled) {
    std::lock_guard<std::mutex> lk(buffer_mu_);
    max_buffer_bytes_ = max_bytes;
    max_buffer_messages_ = max_messages;
    buffer_enabled_ = enabled;
    if (!enabled) {
        // Evict everything so the counts stay honest and memory is released
        // before the app stops subscribing to it.
        while (!buffer_.empty()) {
            stat_buffered_dropped_.fetch_add(1, std::memory_order_relaxed);
            buffer_bytes_ -= buffer_.front().env.data.size();
            buffer_.pop_front();
        }
    }
}

}  // namespace uninet
