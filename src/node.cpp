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
    if (watching_ || !transport_ || !transport_->connected()) return;
    transport_->subscribe(">", [this](const std::string& subject, const Bytes& payload) {
        this->on_raw_(subject, payload);
    });
    watching_ = true;
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

bool Node::publish(const std::string& subject, Cbor data, const std::string& dst_uuid) {
    if (!transport_ || !transport_->connected()) return false;
    Envelope env;
    env.compression = compress_;
    env.src_uuid = uuid_;
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
    std::lock_guard<std::mutex> lk(handlers_mu_);
    handlers_.emplace_back(subject, std::move(handler));
}

void Node::on_raw_(const std::string& subject, const Bytes& payload) {
    // Cheap routing filter BEFORE any decompress/decode: read src/dst from the
    // clear header and drop this peer's own echo (and frames not addressed to it)
    // without touching the compressed payload. With LZ4/zlib on, this is what
    // keeps a publisher from decoding every frame it sends.
    auto route = peek_routing(payload);
    if (!route) return;
    if (route->src == uuid_) return;                          // own echo
    if (!route->dst.empty() && route->dst != uuid_) return;   // not addressed to me

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
    std::vector<std::pair<std::string, DataHandler>> snapshot;
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        for (auto& [pat, h] : handlers_)
            if (subject_matches(pat, subject)) snapshot.emplace_back(pat, h);
    }
    for (auto& [pat, h] : snapshot) {
        // Per handler, not per message: without this, the first subscriber that
        // threw silently cancelled delivery to every subscriber after it, and
        // the exception was swallowed further up by the transport.
        (void)pat;
        if (!h) continue;
        try {
            h(*env);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "uninet: exception in a subscriber for '%s': %s\n",
                         subject.c_str(), e.what());
        } catch (...) {
            std::fprintf(stderr, "uninet: unknown exception in a subscriber for '%s'\n",
                         subject.c_str());
        }
    }
}

}  // namespace uninet
