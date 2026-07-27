// UniNet: ZRE transport implementation. See include/uninet/zyre_transport.h.
//
// Zyre's socket, like every ZeroMQ socket, belongs to exactly one thread. So a
// single background thread owns it and runs the whole event loop, and every
// public method that needs to touch Zyre posts a command to that thread over an
// inproc pipe instead of reaching for the socket itself. That is the standard
// CZMQ actor pattern, and it means there is no lock around the network at all -
// publishing from ten threads costs ten inproc sends, not ten contended mutexes.
#include "uninet/zyre_transport.h"

#include <czmq.h>
#include <zyre.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

namespace uninet {

std::string Peer::endpoint() const {
    if (address.empty()) return "";
    std::string a = address;
    const size_t scheme = a.find("://");
    if (scheme != std::string::npos) a = a.substr(scheme + 3);
    const size_t colon = a.rfind(':');
    if (colon != std::string::npos) a = a.substr(0, colon);
    return a;
}

std::string Peer::describe() const {
    std::string s = name.empty() ? uuid : name;
    const std::string where = endpoint();
    if (!where.empty()) s += " (" + where + ")";
    const std::string r = role();
    if (!r.empty()) s += ": " + r;
    return s;
}

std::string local_hostname() {
    char buf[256] = {0};
    if (gethostname(buf, sizeof buf - 1) != 0) return "";
    // A hostname lands in device lists and terminal output; strip anything that
    // could move a cursor or forge a line there.
    std::string out;
    for (char c : std::string(buf)) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) continue;
        out.push_back(c);
        if (out.size() >= 96) break;
    }
    return out;
}

std::string zyre_version_string() {
    char buf[96];
    std::snprintf(buf, sizeof buf, "zyre %d.%d.%d / czmq %d.%d.%d / zmq %d.%d.%d",
                  ZYRE_VERSION_MAJOR, ZYRE_VERSION_MINOR, ZYRE_VERSION_PATCH,
                  CZMQ_VERSION_MAJOR, CZMQ_VERSION_MINOR, CZMQ_VERSION_PATCH,
                  ZMQ_VERSION_MAJOR, ZMQ_VERSION_MINOR, ZMQ_VERSION_PATCH);
    return buf;
}

// ── implementation ────────────────────────────────────────────────────────

struct ZyreTransport::Impl {
    std::string  name;
    ZyreConfig   cfg;
    zyre_t*      node = nullptr;
    zactor_t*    actor = nullptr;

    std::string  uuid;
    std::string  error;

    // Guards everything below, which the actor thread writes and callers read.
    mutable std::mutex mu;
    // Peers that have joined OUR realm: the ones callers see.
    std::map<std::string, Peer> peers;
    // Peers seen on the wire but not (yet) in our realm. ZRE's ENTER fires for
    // every node sharing the beacon port, so a dev laptop in another realm shows
    // up here and never graduates to `peers`. Keeping the record means that when
    // a peer does join our realm we already have its name, address and headers
    // and can report it complete, in one event.
    std::map<std::string, Peer> seen;
    std::vector<std::pair<std::string, MessageHandler>> subs;
    PeerCallback on_found, on_lost;
    std::atomic<bool> running{false};
    // The actor thread's id, so disconnect() can tell whether it is being
    // called from inside a callback. Joining from there deadlocks: every
    // callback runs ON this thread.
    std::atomic<std::thread::id> actor_thread{};

    // Guards sends into the actor pipe: zactor_t is a zsock, so it is not safe
    // to write from two threads at once.
    std::mutex pipe_mu;

    void set_error(const std::string& e) {
        std::lock_guard<std::mutex> lk(mu);
        error = e;
    }

    // Post a command to the actor thread. Frames: [verb][arg...][payload].
    bool send_cmd(zmsg_t** msg) {
        std::lock_guard<std::mutex> lk(pipe_mu);
        if (!actor || !running.load()) { zmsg_destroy(msg); return false; }
        return zmsg_send(msg, actor) == 0;
    }

    // Deliver a received message to every matching subscriber. Handlers run
    // outside the lock so one that publishes (a reply) cannot deadlock.
    void dispatch(const std::string& subject, const Bytes& payload) {
        std::vector<MessageHandler> matched;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto& s : subs)
                if (subject_matches(s.first, subject)) matched.push_back(s.second);
        }
        for (auto& h : matched) {
            // One throwing subscriber must not cancel delivery to the others,
            // and must never unwind into Zyre's C frames.
            try { if (h) h(subject, payload); } catch (...) {}
        }
    }

    // ENTER: a ZRE node appeared on the beacon port. It is NOT yet one of ours -
    // every realm shares the port, so we only record what we learned and wait for
    // a JOIN naming our realm.
    void peer_entered(const char* uuid_c, const char* name_c, const char* addr_c,
                      zhash_t* headers) {
        if (!uuid_c) return;
        Peer p;
        p.uuid    = uuid_c;
        p.name    = name_c ? name_c : "";
        p.address = addr_c ? addr_c : "";
        if (headers) {
            for (const char* v = static_cast<const char*>(zhash_first(headers)); v;
                 v = static_cast<const char*>(zhash_next(headers))) {
                const char* key = zhash_cursor(headers);
                if (key) p.headers[key] = v;
            }
        }
        std::lock_guard<std::mutex> lk(mu);
        // If JOIN arrived before ENTER the visible entry has only a uuid, and
        // nothing refreshed it afterwards, so peers() reported a nameless
        // address for the rest of the run. Fill it in now.
        auto vis = peers.find(p.uuid);
        if (vis != peers.end()) vis->second = p;
        seen[p.uuid] = std::move(p);
    }

    // JOIN: the peer entered a group. If it is our realm, it is now visible.
    void peer_joined(const char* uuid_c, const char* group_c) {
        if (!uuid_c || !group_c || cfg.realm != group_c) return;
        Peer p;
        PeerCallback cb;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (peers.count(uuid_c)) return;          // already visible
            auto it = seen.find(uuid_c);
            if (it != seen.end()) {
                p = it->second;
            } else {
                p.uuid = uuid_c;                      // JOIN before ENTER: rare, survivable
            }
            peers[p.uuid] = p;
            cb = on_found;
        }
        if (cb) { try { cb(p); } catch (...) {} }
    }

    // LEAVE our realm, or EXIT the network entirely: either way the peer stops
    // being visible. `forget` distinguishes the two, a peer that merely left the
    // group is still on the network and keeps its record.
    void peer_gone(const char* uuid_c, bool forget) {
        if (!uuid_c) return;
        Peer gone;
        PeerCallback cb;
        bool was_visible = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (forget) seen.erase(uuid_c);
            auto it = peers.find(uuid_c);
            if (it != peers.end()) {
                gone = it->second;
                peers.erase(it);
                was_visible = true;
                cb = on_lost;
            }
        }
        if (was_visible && cb) { try { cb(gone); } catch (...) {} }
    }

    void handle_event(zyre_event_t* ev) {
        const char* type = zyre_event_type(ev);
        if (!type) return;

        if (std::strcmp(type, "ENTER") == 0) {
            peer_entered(zyre_event_peer_uuid(ev), zyre_event_peer_name(ev),
                         zyre_event_peer_addr(ev), zyre_event_headers(ev));
        } else if (std::strcmp(type, "JOIN") == 0) {
            peer_joined(zyre_event_peer_uuid(ev), zyre_event_group(ev));
        } else if (std::strcmp(type, "LEAVE") == 0) {
            const char* g = zyre_event_group(ev);
            if (g && cfg.realm == g) peer_gone(zyre_event_peer_uuid(ev), false);
        } else if (std::strcmp(type, "EXIT") == 0) {
            peer_gone(zyre_event_peer_uuid(ev), true);
        } else if (std::strcmp(type, "SHOUT") == 0 || std::strcmp(type, "WHISPER") == 0) {
            // Wire shape: [subject][payload]. Keeping the subject in its own
            // frame lets a receiver match subscriptions without decompressing
            // or decoding anything.
            zmsg_t* m = zyre_event_msg(ev);
            if (!m || zmsg_size(m) < 2) return;
            char* subj = zmsg_popstr(m);
            zframe_t* body = zmsg_pop(m);
            if (subj && body) {
                const uint8_t* d = zframe_data(body);
                Bytes payload(d, d + zframe_size(body));
                dispatch(subj, payload);
            }
            freen(subj);
            zframe_destroy(&body);
        }
        // EVASIVE / SILENT are ZRE's "peer is slow to answer" warnings. A peer
        // that is genuinely gone produces EXIT, which is what removes it: so
        // a device is not dropped from the list for one late heartbeat.
    }

    // The actor thread. Owns the Zyre socket exclusively for its whole life.
    static void actor_fn(zsock_t* pipe, void* arg) {
        auto* self = static_cast<Impl*>(arg);
        self->actor_thread.store(std::this_thread::get_id());
        zsock_signal(pipe, 0);                      // tell the constructor we are up

        zpoller_t* poller = zpoller_new(pipe, zyre_socket(self->node), nullptr);
        bool terminated = false;

        while (!terminated && !zsys_interrupted) {
            void* which = zpoller_wait(poller, 250);
            if (zpoller_terminated(poller)) break;

            if (which == pipe) {
                zmsg_t* msg = zmsg_recv(pipe);
                if (!msg) break;
                char* verb = zmsg_popstr(msg);
                if (!verb) { zmsg_destroy(&msg); continue; }

                if (std::strcmp(verb, "$TERM") == 0) {
                    terminated = true;
                } else if (std::strcmp(verb, "SHOUT") == 0) {
                    char* subject = zmsg_popstr(msg);
                    zframe_t* body = zmsg_pop(msg);
                    if (subject && body) {
                        zmsg_t* out = zmsg_new();
                        zmsg_addstr(out, subject);
                        zmsg_append(out, &body);
                        zyre_shout(self->node, self->cfg.realm.c_str(), &out);
                    }
                    freen(subject);
                    zframe_destroy(&body);
                } else if (std::strcmp(verb, "WHISPER") == 0) {
                    char* peer = zmsg_popstr(msg);
                    char* subject = zmsg_popstr(msg);
                    zframe_t* body = zmsg_pop(msg);
                    if (peer && subject && body) {
                        zmsg_t* out = zmsg_new();
                        zmsg_addstr(out, subject);
                        zmsg_append(out, &body);
                        zyre_whisper(self->node, peer, &out);
                    }
                    freen(peer);
                    freen(subject);
                    zframe_destroy(&body);
                }
                freen(verb);
                zmsg_destroy(&msg);
            } else if (which) {
                zyre_event_t* ev = zyre_event_new(self->node);
                if (ev) {
                    self->handle_event(ev);
                    zyre_event_destroy(&ev);
                }
            }
        }
        zpoller_destroy(&poller);
    }
};

// ── public surface ────────────────────────────────────────────────────────

ZyreTransport::ZyreTransport(std::string name, ZyreConfig cfg) : impl_(new Impl()) {
    impl_->name = std::move(name);
    impl_->cfg  = std::move(cfg);

    // Keep CZMQ from installing its own SIGINT/SIGTERM handlers: UniNet is a
    // library, and an application's signal handling is not ours to take over.
    zsys_handler_set(nullptr);

    impl_->node = zyre_new(impl_->name.c_str());
    if (impl_->node) {
        impl_->uuid = zyre_uuid(impl_->node);
        // Advertised with the beacon, so a peer knows what we are the moment it
        // sees us, no round-trip needed to build a device list.
        zyre_set_header(impl_->node, "app", "%s", "uninet");
        for (const auto& kv : impl_->cfg.headers)
            zyre_set_header(impl_->node, kv.first.c_str(), "%s", kv.second.c_str());
    } else {
        impl_->set_error("could not create a ZRE node (is the network available?)");
    }
}

ZyreTransport::~ZyreTransport() {
    disconnect();
    if (impl_->node) zyre_destroy(&impl_->node);
}

bool ZyreTransport::set_header(const std::string& key, const std::string& value) {
    if (impl_->running.load()) {
        // ZRE sends headers once, with the discovery beacon, so a later change
        // would never reach anyone. Say so instead of discarding it silently.
        impl_->set_error("set_header('" + key + "') was called after connect(); "
                         "ZRE sends headers once with the beacon, so it was ignored");
        return false;
    }
    impl_->cfg.headers[key] = value;
    if (impl_->node) zyre_set_header(impl_->node, key.c_str(), "%s", value.c_str());
    return true;
}

bool ZyreTransport::connect() {
    if (impl_->running.load()) return true;
    if (!impl_->node) return false;
    {   // A previous failure must not be reported as this attempt's reason.
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->error.clear();
    }

    // Gossip mode replaces the UDP beacon entirely, so the beacon settings below
    // do not apply to it. Setting the endpoint is what switches ZRE over.
    const bool gossip = !impl_->cfg.gossip_bind.empty() ||
                        !impl_->cfg.gossip_connect.empty();
    if (gossip) {
        // ZRE requires an explicit endpoint in gossip mode: with no beacon there
        // is nothing to announce an ephemeral port. Pick a sane default rather
        // than failing, so the common case needs one setting, not three.
        const std::string ep = impl_->cfg.endpoint.empty()
                                   ? std::string("tcp://*:") + std::to_string(impl_->cfg.port + 1)
                                   : impl_->cfg.endpoint;
        if (zyre_set_endpoint(impl_->node, "%s", ep.c_str()) != 0) {
            impl_->set_error("could not bind the node endpoint '" + ep + "'");
            return false;
        }
        if (!impl_->cfg.advertised_endpoint.empty()) {
            // zyre_set_advertised_endpoint is a DRAFT API: it exists only when
            // Zyre was built with -DENABLE_DRAFTS. Distro packages usually are
            // not, so guard it and say so rather than failing to compile there.
#ifdef ZYRE_BUILD_DRAFT_API
            zyre_set_advertised_endpoint(impl_->node,
                                         impl_->cfg.advertised_endpoint.c_str());
#else
            impl_->set_error(
                "advertised_endpoint needs a Zyre built with -DENABLE_DRAFTS; "
                "this build has the stable API only, so the setting is ignored");
#endif
        }
        if (!impl_->cfg.gossip_bind.empty())
            zyre_gossip_bind(impl_->node, "%s", impl_->cfg.gossip_bind.c_str());
        if (!impl_->cfg.gossip_connect.empty())
            zyre_gossip_connect(impl_->node, "%s", impl_->cfg.gossip_connect.c_str());
    } else if (impl_->cfg.port > 0) {
        zyre_set_port(impl_->node, impl_->cfg.port);
    }
    if (!impl_->cfg.iface.empty())
        zyre_set_interface(impl_->node, impl_->cfg.iface.c_str());
    zyre_set_evasive_timeout(impl_->node, impl_->cfg.evasive_ms);
    zyre_set_expired_timeout(impl_->node, impl_->cfg.expired_ms);

    if (zyre_start(impl_->node) != 0) {
        impl_->set_error("could not start discovery: another program may hold UDP port "
                         + std::to_string(impl_->cfg.port));
        return false;
    }
    if (zyre_join(impl_->node, impl_->cfg.realm.c_str()) != 0) {
        impl_->set_error("could not join realm '" + impl_->cfg.realm + "'");
        zyre_stop(impl_->node);
        return false;
    }

    impl_->running.store(true);
    // zactor_new blocks until actor_fn signals, so the poller is live before we
    // return and no early beacon is missed.
    impl_->actor = zactor_new(Impl::actor_fn, impl_.get());
    if (!impl_->actor) {
        impl_->running.store(false);
        impl_->set_error("could not start the network thread");
        zyre_stop(impl_->node);
        return false;
    }
    return true;
}

void ZyreTransport::disconnect() {
    // Calling this from a callback would join the thread we are running on.
    // Refusing loudly beats hanging: the caller gets an explanation from
    // last_error() instead of a process that never returns.
    if (impl_->actor_thread.load() == std::this_thread::get_id()) {
        impl_->set_error("disconnect() was called from a UniNet callback, which "
                         "would deadlock; close the session from another thread");
        return;
    }
    if (!impl_->running.exchange(false)) return;
    // Destroying the actor sends $TERM and joins the thread, so the event loop
    // is quiet before we touch the Zyre node again.
    {
        std::lock_guard<std::mutex> lk(impl_->pipe_mu);
        if (impl_->actor) zactor_destroy(&impl_->actor);
    }
    if (impl_->node) zyre_stop(impl_->node);   // sends EXIT: peers drop us at once

    // Report the departures instead of silently emptying the table: an app that
    // tracks devices purely through found/lost would keep a stale list forever.
    std::vector<Peer> gone;
    PeerCallback cb;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& kv : impl_->peers) gone.push_back(kv.second);
        impl_->peers.clear();
        impl_->seen.clear();     // no EXIT will arrive while disconnected
        cb = impl_->on_lost;
    }
    if (cb) for (const auto& p : gone) { try { cb(p); } catch (...) {} }
}

bool ZyreTransport::connected() const { return impl_->running.load(); }

bool ZyreTransport::publish(const std::string& subject, const uint8_t* data, size_t len) {
    if (!impl_->running.load()) return false;
    zmsg_t* m = zmsg_new();
    zmsg_addstr(m, "SHOUT");
    zmsg_addstr(m, subject.c_str());
    zmsg_addmem(m, data, len);
    return impl_->send_cmd(&m);
}

bool ZyreTransport::publish_to(const std::string& peer_uuid, const std::string& subject,
                               const uint8_t* data, size_t len) {
    if (!impl_->running.load() || peer_uuid.empty()) return false;
    // Zyre drops a whisper to a peer it does not know, and the actor thread has
    // no way to report that back. Returning false here lets Node fall back to a
    // broadcast the intended peer can still filter, instead of the message
    // disappearing while publish() reports success.
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->peers.count(peer_uuid)) return false;
    }
    zmsg_t* m = zmsg_new();
    zmsg_addstr(m, "WHISPER");
    zmsg_addstr(m, peer_uuid.c_str());
    zmsg_addstr(m, subject.c_str());
    zmsg_addmem(m, data, len);
    return impl_->send_cmd(&m);
}

void ZyreTransport::subscribe(const std::string& subject, MessageHandler handler) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->subs.emplace_back(subject, std::move(handler));
}

void ZyreTransport::unsubscribe(const std::string& subject) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (auto it = impl_->subs.begin(); it != impl_->subs.end();)
        it = (it->first == subject) ? impl_->subs.erase(it) : it + 1;
}

std::vector<Peer> ZyreTransport::peers() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    std::vector<Peer> out;
    out.reserve(impl_->peers.size());
    for (const auto& kv : impl_->peers) out.push_back(kv.second);
    return out;
}

void ZyreTransport::on_peer_found(PeerCallback cb) {
    // Replay the peers already known, so a caller that registers after connect()
    // still learns about everyone: otherwise "who is here" silently depends on
    // whether you registered before or after the first beacon.
    std::vector<Peer> known;
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->on_found = cb;
        for (const auto& kv : impl_->peers) known.push_back(kv.second);
    }
    if (cb) for (const auto& p : known) { try { cb(p); } catch (...) {} }
}

void ZyreTransport::on_peer_lost(PeerCallback cb) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->on_lost = std::move(cb);
}

const std::string& ZyreTransport::uuid() const { return impl_->uuid; }
const std::string& ZyreTransport::node_name() const { return impl_->name; }

std::string ZyreTransport::last_error() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->error;
}

}  // namespace uninet
