// UniNet: large payload transfer. See include/uninet/blob.h.
#include "uninet/blob.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <utility>

namespace uninet {
namespace {

// Wire shape, inside the normal envelope so it inherits compression and routing:
//   { "k": "b"|"c"|"e", "id": text, ... }
// "b" begin  -> name, size, meta
// "c" chunk  -> seq (uint), d (bytes)
// "e" end    -> (nothing beyond id; the receiver already knows the size)
constexpr char kBegin = 'b';
constexpr char kChunk = 'c';
constexpr char kEnd   = 'e';

std::string basename_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Unique enough without pulling in a UUID library: the sender's own uuid already
// scopes it, so a per-process counter finishes the job.
std::string next_id(const std::string& src) {
    static std::atomic<uint64_t> n{0};
    return src + "/" + std::to_string(n.fetch_add(1));
}

}  // namespace

struct Blob::Impl {
    Session&    session;
    std::string subject;
    BlobConfig  cfg;

    struct Incoming {
        BlobInfo info;
        Bytes    data;
        size_t   received = 0;
        uint64_t next_seq = 0;
        std::chrono::steady_clock::time_point last_chunk;
    };

    mutable std::mutex mu;
    std::map<std::string, Incoming> incoming;
    size_t buffered = 0;          // total bytes held across all transfers

    BlobHandler  on_done;
    BlobProgress on_progress;
    BlobFailed   on_failed;

    Impl(Session& s, std::string subj, BlobConfig c)
        : session(s), subject(std::move(subj)), cfg(std::move(c)) {}

    void fail(const BlobInfo& info, const std::string& why) {
        BlobFailed cb;
        {
            std::lock_guard<std::mutex> lk(mu);
            cb = on_failed;
        }
        if (cb) { try { cb(info, why); } catch (...) {} }
    }

    // Abandon transfers that have gone quiet. Caller must NOT hold `mu`.
    void sweep_locked_free() {
        std::vector<std::pair<BlobInfo, std::string>> dropped;
        {
            std::lock_guard<std::mutex> lk(mu);
            const auto now = std::chrono::steady_clock::now();
            for (auto it = incoming.begin(); it != incoming.end();) {
                if (now - it->second.last_chunk > cfg.stall_timeout) {
                    buffered -= it->second.data.size();
                    dropped.emplace_back(it->second.info, "transfer stalled");
                    it = incoming.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& d : dropped) fail(d.first, d.second);
    }

    void handle(const Envelope& env) {
        const Cbor& m = env.data;
        if (!m.is_map()) return;
        const Cbor& kind = m["k"];
        const Cbor& id_v = m["id"];
        if (!kind.is_text() || kind.as_text().empty() || !id_v.is_text()) return;
        const char k = kind.as_text()[0];
        const std::string& id = id_v.as_text();

        if (k == kBegin) {
            const Cbor& size_v = m["size"];
            if (!size_v.is_uint()) return;
            const size_t size = size_t(size_v.as_uint());

            BlobInfo info;
            info.id   = id;
            info.name = m["name"].is_text() ? m["name"].as_text() : std::string{};
            info.src  = env.src_uuid;
            info.meta = m["meta"];
            info.size = size;

            // Refuse rather than allocate: every limit here exists because the
            // sender is an unauthenticated peer on the LAN.
            std::string why;
            {
                std::lock_guard<std::mutex> lk(mu);
                if (size > cfg.max_blob_bytes)                why = "transfer too large";
                else if (incoming.size() >= cfg.max_concurrent) why = "too many concurrent transfers";
                else if (buffered + size > cfg.max_total_bytes) why = "too much data in flight";
                if (why.empty()) {
                    Incoming in;
                    in.info = info;
                    in.data.resize(size);
                    in.last_chunk = std::chrono::steady_clock::now();
                    buffered += size;
                    incoming[id] = std::move(in);
                }
            }
            if (!why.empty()) fail(info, why);
            return;
        }

        if (k == kChunk) {
            const Cbor& seq_v = m["seq"];
            const Cbor& d     = m["d"];
            if (!seq_v.is_uint() || !d.is_bytes()) return;
            const uint64_t seq = seq_v.as_uint();
            const Bytes& bytes = d.as_bytes();

            BlobInfo info;
            size_t done = 0;
            BlobProgress prog;
            bool bad = false;
            {
                std::lock_guard<std::mutex> lk(mu);
                auto it = incoming.find(id);
                if (it == incoming.end()) return;      // unknown or already dropped
                Incoming& in = it->second;

                // Chunks must arrive in order. ZRE delivers in order per peer, so
                // a gap means loss or interleaving from a second sender reusing an
                // id: either way the buffer can no longer be trusted.
                if (seq != in.next_seq) {
                    bad = true;
                    info = in.info;
                    buffered -= in.data.size();
                    incoming.erase(it);
                } else if (in.received + bytes.size() > in.data.size()) {
                    bad = true;                        // sender overshot its own declared size
                    info = in.info;
                    buffered -= in.data.size();
                    incoming.erase(it);
                } else {
                    std::memcpy(in.data.data() + in.received, bytes.data(), bytes.size());
                    in.received += bytes.size();
                    in.next_seq = seq + 1;
                    in.last_chunk = std::chrono::steady_clock::now();
                    info = in.info;
                    done = in.received;
                    prog = on_progress;
                }
            }
            if (bad) { fail(info, "chunk out of order or oversized"); return; }
            if (prog) { try { prog(info, done); } catch (...) {} }
            return;
        }

        if (k == kEnd) {
            BlobInfo info;
            Bytes data;
            BlobHandler cb;
            bool complete = false, short_ = false;
            {
                std::lock_guard<std::mutex> lk(mu);
                auto it = incoming.find(id);
                if (it == incoming.end()) return;
                Incoming& in = it->second;
                info = in.info;
                buffered -= in.data.size();
                if (in.received == in.data.size()) {
                    data = std::move(in.data);
                    complete = true;
                    cb = on_done;
                } else {
                    short_ = true;
                }
                incoming.erase(it);
            }
            if (short_) { fail(info, "transfer ended early"); return; }
            if (complete && cb) { try { cb(info, data); } catch (...) {} }
        }
    }
};

Blob::Blob(Session& session, std::string subject, BlobConfig cfg)
    : impl_(new Impl(session, std::move(subject), std::move(cfg))) {
    // One subscription for the whole transfer protocol. It sits under the
    // caller's subject so it cannot collide with their own traffic.
    session.subscribe(impl_->subject + ".blob", [this](const Envelope& env) {
        impl_->handle(env);
        impl_->sweep_locked_free();
    });
}

Blob::~Blob() = default;

std::string Blob::send(const std::string& name, const uint8_t* data, size_t len,
                       Cbor meta, const std::string& dst) {
    if (!data && len) return "";
    const std::string topic = impl_->subject + ".blob";
    const std::string id = next_id(impl_->session.uuid());

    Cbor begin = Cbor::map();
    begin.set("k", Cbor::text(std::string(1, kBegin)));
    begin.set("id", Cbor::text(id));
    begin.set("name", Cbor::text(name));
    begin.set("size", Cbor::uint(len));
    begin.set("meta", std::move(meta));
    // If the very first message cannot go out we are not on the network; say so
    // now rather than looping over chunks that will all fail the same way.
    if (!impl_->session.publish(topic, std::move(begin), dst)) return "";

    const size_t chunk = impl_->cfg.chunk_bytes ? impl_->cfg.chunk_bytes : 256 * 1024;
    uint64_t seq = 0;
    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = (len - off < chunk) ? (len - off) : chunk;
        Cbor c = Cbor::map();
        c.set("k", Cbor::text(std::string(1, kChunk)));
        c.set("id", Cbor::text(id));
        c.set("seq", Cbor::uint(seq++));
        c.set("d", Cbor::bytes(Bytes(data + off, data + off + n)));
        // Abandon on the first failure. Sending the remaining chunks would only
        // leave the receiver holding a buffer it can never complete, which its
        // stall timeout would eventually reap, but slowly, and confusingly.
        if (!impl_->session.publish(topic, std::move(c), dst)) return "";
    }

    Cbor end = Cbor::map();
    end.set("k", Cbor::text(std::string(1, kEnd)));
    end.set("id", Cbor::text(id));
    if (!impl_->session.publish(topic, std::move(end), dst)) return "";
    return id;
}

std::string Blob::send_file(const std::string& path, Cbor meta,
                            const std::string& dst, const std::string& name) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return "";
    const std::streamsize size = in.tellg();
    if (size < 0) return "";
    in.seekg(0, std::ios::beg);

    // Braces, not parentheses: `Bytes data(size_t(size))` is a function
    // declaration, not a vector.
    Bytes data(static_cast<size_t>(size), 0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) return "";

    return send(name.empty() ? basename_of(path) : name,
                data.data(), data.size(), std::move(meta), dst);
}

void Blob::on_received(BlobHandler cb) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->on_done = std::move(cb);
}

void Blob::on_progress(BlobProgress cb) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->on_progress = std::move(cb);
}

void Blob::on_failed(BlobFailed cb) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->on_failed = std::move(cb);
}

size_t Blob::incoming_count() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->incoming.size();
}

void Blob::sweep() { impl_->sweep_locked_free(); }

}  // namespace uninet
