// UniNet — Node implementation (the high-level peer).
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

std::string make_uuid(const std::string& name) {
    using clock = std::chrono::system_clock;
    std::time_t now = clock::to_time_t(clock::now());
    std::tm tm{};
    gmtime_r(&now, &tm);
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
    if (transport_) {
        // One raw subscription per Node: it unfames every frame and applies the
        // protocol-level filters (echo suppression, dst targeting) once, here.
        transport_->subscribe(">", [this](const std::string& subject, const Bytes& payload) {
            this->on_raw_(subject, payload);
        });
    }
}

bool Node::connect() {
    return transport_ && transport_->connect();
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

void Node::publish(const std::string& subject, Cbor data, const std::string& dst_uuid) {
    if (!transport_ || !transport_->connected()) return;
    Envelope env;
    env.compression = compress_;
    env.src_uuid = uuid_;
    env.dst_uuid = dst_uuid;
    env.subject = subject;
    env.data = std::move(data);
    profiler::ScopedOp _("node.publish");
    Bytes wire = frame(env);
    _.set_bytes_in(wire.size());
    _.set_bytes_out(wire.size());
    transport_->publish(subject, wire.data(), wire.size());
}

void Node::subscribe(const std::string& subject, DataHandler handler) {
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

    auto env = unframe(payload);
    if (!env) return;
    if (env->protocol_version != CURRENT_PROTOCOL_VERSION) {
        // Forward-compatible policy: accept the major, ignore unrecognized fields.
    }
    std::vector<std::pair<std::string, DataHandler>> snapshot;
    {
        std::lock_guard<std::mutex> lk(handlers_mu_);
        for (auto& [pat, h] : handlers_)
            if (subject_matches(pat, subject)) snapshot.emplace_back(pat, h);
    }
    for (auto& [pat, h] : snapshot)
        if (h) h(*env);
}

}  // namespace uninet
