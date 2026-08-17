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
#include <cstdio>
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

const char* link_kind_name(LinkKind kind) {
    switch (kind) {
        case LinkKind::Wired:    return "wired";
        case LinkKind::Wireless: return "wi-fi";
        case LinkKind::Tethered: return "usb-tether";
        case LinkKind::Virtual:  return "virtual";
        case LinkKind::Vpn:      return "vpn";
        case LinkKind::Loopback: return "loopback";
        case LinkKind::Unknown:  break;
    }
    return "unknown";
}

namespace {

bool starts_with(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// Classified by name, which is what every platform actually gives us. The
// kernel does expose link types, but not portably, and the naming conventions
// below are stable across Linux, macOS and Windows' friendly names.
LinkKind classify(const std::string& name) {
    if (name == "lo" || starts_with(name, "Loopback")) return LinkKind::Loopback;
    // Before the wired test: systemd names a tethered phone enp0s20u1, which
    // starts with "en" and would otherwise read as ordinary ethernet. The "u"
    // marks a USB path, and that is exactly the case worth singling out.
    if (starts_with(name, "rndis") || starts_with(name, "usb")) return LinkKind::Tethered;
    if (starts_with(name, "en") && name.find('u') != std::string::npos &&
        name.find("usb") == std::string::npos) {
        // enp0s20u1 / enx… : USB-attached ethernet, which is how both a
        // tethered phone and a USB ethernet dongle appear.
        return LinkKind::Tethered;
    }
    if (starts_with(name, "docker") || starts_with(name, "br-") ||
        starts_with(name, "veth")   || starts_with(name, "virbr") ||
        starts_with(name, "vmnet")  || starts_with(name, "bridge") ||
        starts_with(name, "cni")    || starts_with(name, "flannel")) return LinkKind::Virtual;
    if (starts_with(name, "tun") || starts_with(name, "tap") ||
        starts_with(name, "wg")  || starts_with(name, "ppp") ||
        starts_with(name, "utun")) return LinkKind::Vpn;
    if (starts_with(name, "wl") || starts_with(name, "wifi") ||
        starts_with(name, "Wi-Fi") || starts_with(name, "wlan") ||
        starts_with(name, "ath")) return LinkKind::Wireless;
    if (starts_with(name, "en") || starts_with(name, "eth") ||
        starts_with(name, "Ethernet")) return LinkKind::Wired;
    return LinkKind::Unknown;
}

}  // namespace

bool Interface::is_private() const {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(address.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a == 10) return true;                          // 10.0.0.0/8
    if (a == 192 && b == 168) return true;             // 192.168.0.0/16
    if (a == 172 && b >= 16 && b <= 31) return true;   // 172.16.0.0/12
    if (a == 169 && b == 254) return true;             // 169.254.0.0/16 link-local
    return false;
}

bool Interface::is_discoverable() const {
    if (address.empty()) return false;
    switch (kind) {
        // No other UniNet device is ever on the far side of these. A beacon
        // sent here is not merely useless, it is the reason a machine with
        // docker installed finds nothing at all by default.
        case LinkKind::Loopback:
        case LinkKind::Virtual:
        case LinkKind::Vpn:
            return false;
        default:
            return true;
    }
}

Interface best_interface(const std::vector<Interface>& candidates) {
    const Interface* best = nullptr;
    int best_score = -1;
    for (const auto& i : candidates) {
        if (!i.is_discoverable()) continue;
        int score = 0;
        switch (i.kind) {
            // A tethered phone was plugged in deliberately and is almost always
            // the device being reached for, so it outranks everything.
            case LinkKind::Tethered: score = 40; break;
            // Wired above Wi-Fi, deliberately. A machine cabled to a switch is
            // usually cabled to it ON PURPOSE - a lab bench, a navigation cart -
            // while its Wi-Fi is whatever the building offers. Preferring Wi-Fi
            // would quietly move such a setup off the network it was wired to.
            case LinkKind::Wired:    score = 30; break;
            case LinkKind::Wireless: score = 25; break;
            default:                 score = 10; break;
        }
        // This outweighs the link type, and that is the point. A private address
        // is a LAN, a hotspot or a tether: somewhere other UniNet devices
        // plausibly are. A public one is a campus or hospital network where they
        // almost never are, and where a beacon may not even be forwarded. So a
        // cabled public address loses to a private Wi-Fi, while a cabled private
        // network still beats the Wi-Fi beside it.
        if (i.is_private()) score += 20;
        if (score > best_score) { best_score = score; best = &i; }
    }
    return best ? *best : Interface{};
}

std::vector<Interface> local_interfaces() {
    std::vector<Interface> out;
    // czmq's ziflist rather than getifaddrs/GetAdaptersAddresses by hand: it is
    // already a dependency and it is the SAME enumeration Zyre itself uses to
    // choose an interface, so this list cannot disagree with the choice it is
    // meant to explain.
    ziflist_t* list = ziflist_new();
    if (!list) return out;
    for (const char* name = ziflist_first(list); name; name = ziflist_next(list)) {
        Interface iface;
        iface.name = name;
        if (const char* a = ziflist_address(list))   iface.address = a;
        if (const char* b = ziflist_broadcast(list)) iface.broadcast = b;
        if (const char* m = ziflist_netmask(list))   iface.netmask = m;
        iface.kind = classify(iface.name);
        out.push_back(std::move(iface));
    }
    ziflist_destroy(&list);
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
    // The network discovery actually settled on. Guarded by `mu`: a rebuild
    // rewrites it on the actor thread while a caller may be reading it. It used
    // to be lock-free on the grounds that nothing wrote it once the node was
    // running, which stopped being true the moment reconnection was added.
    Interface    chosen_interface;

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

    // ── network supervision ──
    std::function<void()> on_reconnected;
    std::atomic<uint64_t> reconnects{0};
    std::thread watchdog;
    std::atomic<bool> watching{false};

    // Create the ZRE node and put its headers on it. Headers live on the node,
    // so a rebuild has to re-apply them or the device comes back nameless.
    bool build_node() {
        node = zyre_new(name.c_str());
        if (!node) return false;
        {   // Written here by the actor thread on every rebuild, read by
            // uuid() from any caller thread. Unguarded this is a data race, and
            // ThreadSanitizer says so.
            std::lock_guard<std::mutex> lk(mu);
            uuid = zyre_uuid(node);
        }
        zyre_set_header(node, "app", "%s", "uninet");
        for (const auto& kv : cfg.headers)
            zyre_set_header(node, kv.first.c_str(), "%s", kv.second.c_str());
        return true;
    }

    // Everything connect() does between having a node and having a live one.
    // Shared so a reconnect cannot drift from the original path.
    bool configure_and_start(std::string* why) {
        const bool gossip = !cfg.gossip_bind.empty() || !cfg.gossip_connect.empty();
        if (gossip) {
            const std::string ep = cfg.endpoint.empty()
                                       ? std::string("tcp://*:") + std::to_string(cfg.port + 1)
                                       : cfg.endpoint;
            if (zyre_set_endpoint(node, "%s", ep.c_str()) != 0) {
                if (why) *why = "could not bind the node endpoint '" + ep + "'";
                return false;
            }
            if (!cfg.gossip_bind.empty())
                zyre_gossip_bind(node, "%s", cfg.gossip_bind.c_str());
            if (!cfg.gossip_connect.empty())
                zyre_gossip_connect(node, "%s", cfg.gossip_connect.c_str());
        } else if (cfg.port > 0) {
            zyre_set_port(node, cfg.port);
        }
        std::string chosen = cfg.iface;
        if (chosen.empty() && !gossip) {
            const Interface best = best_interface(local_interfaces());
            if (!best.name.empty()) {
                chosen = best.name;
                std::lock_guard<std::mutex> lk(mu);
                chosen_interface = best;
            }
        }
        zyre_set_evasive_timeout(node, cfg.evasive_ms);
        zyre_set_expired_timeout(node, cfg.expired_ms);

        // zyre_set_interface writes czmq's zsys_interface, which is a single
        // process-global string, and zyre_start reads it. Two sessions starting
        // at once in one process is therefore a genuine data race on that
        // string, not merely the documented "last one wins" ambiguity about
        // which interface each ends up on. ThreadSanitizer reported 13 of them
        // from one run of the network test.
        //
        // Serialising set-then-start removes the race. It does NOT make two
        // sessions able to sit on different interfaces: the beacon reads the
        // global later, from Zyre's own thread. That limit is czmq's, and it is
        // why several networks at once needs several processes.
        // Written only when the value actually changes. czmq keeps ONE global
        // interface string for the process and Zyre's own node threads read it
        // whenever they please, so a write always races those reads and no lock
        // on this side can prevent it - their thread will never take our mutex.
        // What we can do is not write the same value over and over: with
        // automatic selection every session would otherwise rewrite it on every
        // connect, and ThreadSanitizer reported 13 races from a single run of
        // the network test. Writing only on a real change leaves one write at
        // startup, before any node thread exists, and one more if the machine
        // genuinely moves network.
        static std::mutex zsys_interface_mu;
        static std::string zsys_interface_now;
        if (!chosen.empty()) {
            std::lock_guard<std::mutex> zsys_lk(zsys_interface_mu);
            if (zsys_interface_now != chosen) {
                zyre_set_interface(node, chosen.c_str());
                zsys_interface_now = chosen;
            }
        }

        if (zyre_start(node) != 0) {
            if (why) *why = "could not start discovery: another program may hold UDP port "
                            + std::to_string(cfg.port);
            return false;
        }
        if (zyre_join(node, cfg.realm.c_str()) != 0) {
            if (why) *why = "could not join realm '" + cfg.realm + "'";
            zyre_stop(node);
            return false;
        }
        return true;
    }

    std::atomic<bool> rebuild_requested{false};

    // Replace the ZRE node with one bound to the current network. Runs ONLY on
    // the actor thread, which is the thread that owns the node.
    void rebuild_node_now() {
        // Peers reachable through the old interface are not reachable any more,
        // whatever happens next. Report them gone now rather than leaving stale
        // entries in the caller's device list; the ones still around will be
        // rediscovered within a beacon interval and reported again.
        std::vector<Peer> gone;
        {
            std::lock_guard<std::mutex> lk(mu);
            for (auto& kv : peers) gone.push_back(kv.second);
            peers.clear();
            seen.clear();
        }
        if (on_lost)
            for (const auto& p : gone) { try { on_lost(p); } catch (...) {} }

        zyre_stop(node);
        zyre_destroy(&node);

        std::string why;
        if (!build_node() || !configure_and_start(&why)) {
            // Left without a node the actor cannot poll anything, so put a
            // stopped one back and let the watchdog try again: a laptop with
            // the lid shut has no network for a while, and that is not fatal.
            set_error(why.empty() ? "could not rebuild the network node" : why);
            if (!node) build_node();
            return;
        }
        reconnects.fetch_add(1, std::memory_order_relaxed);
        if (on_reconnected) { try { on_reconnected(); } catch (...) {} }
    }

    // The network this node should be on right now, or an empty name when the
    // machine has none. An explicit cfg.iface is never second-guessed.
    Interface intended_interface() const {
        const auto all = local_interfaces();
        if (!cfg.iface.empty()) {
            for (const auto& i : all)
                if (i.name == cfg.iface) return i;
            return Interface{};      // named, but currently absent
        }
        return best_interface(all);
    }

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

    // Execute one command whose verb has already been popped off the wire, and
    // destroy *msg. The actor loop and publish() share this so that sending a
    // frame cannot drift between the queued path and the direct path.
    //
    // WHY BOTH PATHS EXIST. A command that goes through the pipe is delivered by
    // the actor loop, which is the only thread allowed to touch the zyre node.
    // But when the CALLER of publish() already IS the actor thread - every
    // subscription handler runs there - the pipe cannot drain until the
    // callback returns, so a publish() made from inside a callback just queues
    // against a consumer that is busy wrong way around. That used to show up as
    // a silent hang: a Blob transfer is one pipe message per chunk, and past the
    // pipe's message HWM (czmq's default zsys_pipehwm value, 1000) zmsg_send
    // blocks forever, the transfer never starts, and nothing is heard from the
    // sender. Executing the command in place instead is safe because
    // zyre_shout/zyre_whisper are still only ever called on the actor thread
    // either way, and the zyre node cannot be touched by anyone else.
    bool run_command(const char* verb, zmsg_t** msg) {
        bool ok = true;
        if (std::strcmp(verb, "SHOUT") == 0) {
            char* subject = zmsg_popstr(*msg);
            zframe_t* body = zmsg_pop(*msg);
            if (subject && body) {
                zmsg_t* out = zmsg_new();
                zmsg_addstr(out, subject);
                zmsg_append(out, &body);
                ok = zyre_shout(node, cfg.realm.c_str(), &out) == 0;
            }
            freen(subject);
            zframe_destroy(&body);
        } else if (std::strcmp(verb, "WHISPER") == 0) {
            char* peer = zmsg_popstr(*msg);
            char* subject = zmsg_popstr(*msg);
            zframe_t* body = zmsg_pop(*msg);
            if (peer && subject && body) {
                zmsg_t* out = zmsg_new();
                zmsg_addstr(out, subject);
                zmsg_append(out, &body);
                zyre_whisper(node, peer, &out);
            }
            freen(peer);
            freen(subject);
            zframe_destroy(&body);
        }
        zmsg_destroy(msg);
        return ok;
    }

    // True when the calling thread is the network thread, i.e. we are inside a
    // subscription handler. publish() must then bypass the pipe (see
    // run_command) or a large blob cannot start.
    bool on_actor_thread() const {
        return actor_thread.load() == std::this_thread::get_id();
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

            // A rebuild replaces the socket the poller is watching, so the
            // poller has to be replaced too. Doing it here, at the top of the
            // loop, keeps the destroy well away from the wait that is using it.
            if (self->rebuild_requested.exchange(false)) {
                self->rebuild_node_now();
                zpoller_destroy(&poller);
                poller = zpoller_new(pipe, zyre_socket(self->node), nullptr);
                if (!poller) break;
                continue;
            }

            if (which == pipe) {
                zmsg_t* msg = zmsg_recv(pipe);
                if (!msg) break;
                char* verb = zmsg_popstr(msg);
                if (!verb) { zmsg_destroy(&msg); continue; }

                if (std::strcmp(verb, "$TERM") == 0) {
                    terminated = true;
                } else {
                    self->run_command(verb, &msg);
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
    const bool gossip = !impl_->cfg.gossip_bind.empty() || !impl_->cfg.gossip_connect.empty();

    // advertised_endpoint is refused rather than ignored: see the note in
    // configure_and_start's gossip branch for why a half-working link is worse
    // than a join that fails and says so.
    if (gossip && !impl_->cfg.advertised_endpoint.empty()) {
#ifdef ZYRE_BUILD_DRAFT_API
        zyre_set_advertised_endpoint(impl_->node, impl_->cfg.advertised_endpoint.c_str());
#else
        impl_->set_error(
            "advertised_endpoint needs a Zyre built with -DENABLE_DRAFTS, "
            "and this one has the stable API only. Either rebuild the "
            "dependency with drafts enabled, or bind `endpoint` directly to "
            "the address peers will use, which needs no draft API");
        return false;
#endif
    }

    std::string why;
    if (!impl_->configure_and_start(&why)) {
        impl_->set_error(why);
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

    // ── the watchdog ──
    // Gossip mode is excluded on purpose: it has no beacon and no bound
    // interface to lose, and its TCP connections are reconnected by ZeroMQ
    // itself. Rebuilding there would drop working links to fix nothing.
    if (impl_->cfg.auto_reconnect && !gossip) {
        impl_->watching.store(true);
        impl_->watchdog = std::thread([impl = impl_.get()] {
            // What the node is on now. A rebuild is warranted when this stops
            // being the right answer: the address changed (a new DHCP lease, a
            // different access point), it vanished (cable out, Wi-Fi off), or a
            // better network appeared (a phone was tethered).
            Interface current;
            {
                std::lock_guard<std::mutex> lk(impl->mu);
                current = impl->chosen_interface;
            }
            while (impl->watching.load()) {
                for (int slept = 0; slept < impl->cfg.reconnect_poll_ms && impl->watching.load();
                     slept += 100)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!impl->watching.load()) break;

                const Interface want = impl->intended_interface();
                // No network at all: nothing to rebuild onto. Waiting beats
                // tearing down a node that may still have loopback peers.
                if (want.name.empty()) continue;
                if (want.name == current.name && want.address == current.address) continue;

                current = want;
                impl->rebuild_requested.store(true);
            }
        });
    }
    return true;
}

void ZyreTransport::on_reconnected(std::function<void()> cb) {
    impl_->on_reconnected = std::move(cb);
}

uint64_t ZyreTransport::reconnect_count() const {
    return impl_->reconnects.load(std::memory_order_relaxed);
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

    // The watchdog goes first. It exists to poke the actor into rebuilding the
    // node; leaving it running while the actor is torn down means it can ask a
    // destroyed node to be replaced.
    impl_->watching.store(false);
    if (impl_->watchdog.joinable()) impl_->watchdog.join();

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
    // Publishing from inside a subscription handler would queue against our own
    // pipe with nobody to drain it; run the command in place instead. See
    // Impl::run_command for why that is safe and why the alternative hangs.
    if (impl_->on_actor_thread()) {
        // run_command expects the verb already removed, exactly as the actor
        // loop hands it over. The direct path must do the same pop itself:
        // passing the full command through would make the verb frame read as
        // the subject and the message would be delivered under the wrong name
        // and silently dropped by every subscriber.
        char* verb = zmsg_popstr(m);
        const bool ok = impl_->run_command(verb, &m);
        freen(verb);
        return ok;
    }
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
    if (impl_->on_actor_thread()) {
        char* verb = zmsg_popstr(m);
        const bool ok = impl_->run_command(verb, &m);
        freen(verb);
        return ok;
    }
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

std::string ZyreTransport::uuid() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->uuid;
}
const std::string& ZyreTransport::node_name() const { return impl_->name; }

std::string ZyreTransport::last_error() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->error;
}

Interface ZyreTransport::chosen_interface() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->chosen_interface;
}

}  // namespace uninet
