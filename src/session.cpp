// UniNet: Session implementation. See include/uninet/session.h.
#include "uninet/session.h"

#include "uninet/json.h"

// Declared here rather than in a public header: the registry is an
// implementation detail between Session and diagnostics.cpp.
namespace uninet {
void diagnostics_register(const Session*);
void diagnostics_unregister(const Session*);
void diagnostics_refresh();
}

#include <czmq.h>
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>
#include <random>

#include <atomic>
#include <memory>
#include <thread>
#include <deque>
#include <shared_mutex>
#include <unordered_set>
#include <stdexcept>
#include <utility>

namespace uninet {

// One extra network, and the helper process that owns it.
//
// The helper exists because czmq keeps ONE interface string for the process, so
// a second node here could not be put on a different network however it was
// configured. See tools/uninet_bridge.cpp.
struct BridgeLink {
    std::string iface;
    std::string endpoint;      // the ipc:// PAIR this parent bound
    zsock_t*    sock = nullptr;
    pid_t       pid  = -1;
    // zsock is not thread-safe, and both publish() (any thread) and the relay
    // thread send on it. A mutex is enough: ZeroMQ allows a socket to move
    // between threads across a full memory barrier, which a mutex provides.
    std::mutex  send_mu;
};

struct Session::Impl {
    std::string name;
    std::unique_ptr<ZyreTransport> transport;
    std::unique_ptr<Node>          node;

    // ── bridging ──
    std::vector<std::unique_ptr<BridgeLink>> bridges;
    std::thread relay;
    std::atomic<bool> relaying{false};

    // Ids this parent has already handled, so a message that comes back around
    // through another bridge is dropped. Bounded FIFO, same reasoning as the
    // helper's: it only has to remember for as long as a loop takes.
    std::mutex seen_mu;
    std::unordered_set<std::string> seen;
    std::deque<std::string> seen_order;

    bool seen_before(const std::string& id) {
        if (id.empty()) return false;
        std::lock_guard<std::mutex> lk(seen_mu);
        if (seen.count(id)) return true;
        seen.insert(id);
        seen_order.push_back(id);
        if (seen_order.size() > 8192) {
            seen.erase(seen_order.front());
            seen_order.pop_front();
        }
        return false;
    }

    // Hand a message to every bridge except the one it arrived from.
    void fan_out(const std::string& subject, const Cbor& data, const std::string& mid,
                 const BridgeLink* from) {
        if (bridges.empty()) return;
        const Bytes body = encode(data);
        for (auto& b : bridges) {
            if (b.get() == from) continue;      // never back where it came from
            std::lock_guard<std::mutex> lk(b->send_mu);
            if (!b->sock) continue;
            zmsg_t* m = zmsg_new();
            zmsg_addstr(m, subject.c_str());
            zmsg_addstr(m, mid.c_str());
            zmsg_addmem(m, body.data(), body.size());
            zmsg_send(&m, b->sock);
        }
    }

    // close() resets these while another thread may be inside publish() or
    // peers(). The null checks alone are a time-of-check/time-of-use race:
    // AddressSanitizer showed a heap-use-after-free with a publisher thread
    // reading Node after close() had freed it. close() takes this exclusively;
    // every accessor takes it shared, so readers still run concurrently.
    mutable std::shared_timed_mutex mu;
};

namespace {

// Where the helper is. Next to the running executable first, because that is
// where a build and an install both put it, then PATH.
std::string find_bridge_helper(const std::string& configured) {
    if (!configured.empty()) return configured;
#ifndef _WIN32
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string dir(buf);
        const size_t slash = dir.rfind('/');
        if (slash != std::string::npos) {
            const std::string candidate = dir.substr(0, slash) + "/uninet-bridge";
            if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
    }
#endif
    return "uninet-bridge";    // let the OS search PATH
}

// The networks worth bridging: everything discoverable except the one the
// session is already on. Virtual and VPN links are excluded by
// is_discoverable(), so a machine with Docker does not bridge into a container
// network it shares with nothing.
std::vector<Interface> bridgeable(const Interface& primary, bool allow_usb) {
    std::vector<Interface> out;
    for (const auto& i : local_interfaces()) {
        if (!i.is_discoverable()) continue;
        if (i.name == primary.name) continue;
        if (!allow_usb && i.kind == LinkKind::Tethered) continue;
        out.push_back(i);
    }
    return out;
}

// A fresh message id. Same shape as Node's, minted here so the local copy and
// every bridged copy share one identity.
std::string make_message_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const uint64_t v = rng();
    static const char* hex = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 0; i < 16; ++i) out[size_t(i)] = hex[(v >> (i * 4)) & 0xF];
    return out;
}

}  // namespace

Session::Session() : impl_(new Impl()) {}

Session::~Session() { close(); }

// Stop the relay thread and every helper. Idempotent.
void Session::stop_bridges() {
    if (!impl_->relaying.exchange(false)) {
        // Never started; still make sure nothing is left behind.
        for (auto& b : impl_->bridges) {
            if (b->pid > 0) { ::kill(b->pid, SIGTERM); int st = 0; ::waitpid(b->pid, &st, 0); }
            if (b->sock) zsock_destroy(&b->sock);
        }
        impl_->bridges.clear();
        return;
    }
    if (impl_->relay.joinable()) impl_->relay.join();
    for (auto& b : impl_->bridges) {
        // SIGTERM, not SIGKILL: the helper closes its session on the way out so
        // its peers see it leave at once instead of timing it out.
        if (b->pid > 0) { ::kill(b->pid, SIGTERM); int st = 0; ::waitpid(b->pid, &st, 0); }
        if (b->sock) zsock_destroy(&b->sock);
        // ipc endpoints are files; leaving them behind litters /tmp.
        if (b->endpoint.rfind("ipc://", 0) == 0)
            ::unlink(b->endpoint.substr(6).c_str());
    }
    impl_->bridges.clear();
}

void Session::close() {
    std::unique_ptr<ZyreTransport> transport;
    std::unique_ptr<Node> node;
    {
        std::unique_lock<std::shared_timed_mutex> lk(impl_->mu);
        // Order matters: Node holds a raw Transport* and a subscription that
        // calls back into it, so the Node goes first. Both are moved out and
        // destroyed after the lock is released, because ~ZyreTransport joins the
        // network thread and holding an exclusive lock across that would block
        // every reader for the duration.
        node = std::move(impl_->node);
        transport = std::move(impl_->transport);
    }
    // The bridges first: their relay thread publishes into the Node, so it has
    // to be stopped before the Node goes away.
    stop_bridges();

    // Stop the network before EITHER is destroyed.
    //
    // The transport's reconnect callback holds a raw Node*, and locals are
    // destroyed in reverse order of declaration: `node` above is declared last,
    // so it dies FIRST, while the transport's watchdog thread is still running
    // and still able to fire that callback. Disconnecting here joins the
    // watchdog and the actor, so nothing can call into the Node afterwards.
    if (transport) transport->disconnect();
    diagnostics_unregister(this);
}

size_t Session::bridge_count() const {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    return impl_->bridges.size();
}

bool Session::open() const {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    return impl_->transport != nullptr;
}

std::unique_ptr<Session> Session::join(const std::string& name, SessionConfig cfg) {
    std::unique_ptr<Session> s(new Session());
    s->impl_->name = name;

    ZyreConfig zcfg;
    zcfg.realm     = cfg.realm;
    zcfg.port      = cfg.port;
    zcfg.iface = cfg.iface;
    zcfg.gossip_bind          = cfg.gossip_bind;
    zcfg.gossip_connect       = cfg.gossip_connect;
    zcfg.endpoint             = cfg.endpoint;
    zcfg.advertised_endpoint  = cfg.advertised_endpoint;
    zcfg.auto_reconnect       = cfg.auto_reconnect;
    zcfg.reconnect_poll_ms    = cfg.reconnect_poll_ms;
    zcfg.headers   = cfg.headers;
    // Role and app ride the discovery beacon, so a peer list is complete the
    // moment a device appears, no follow-up query to ask what it is.
    if (!cfg.role.empty()) zcfg.headers["role"] = cfg.role;
    if (!cfg.app.empty())  zcfg.headers["app"]  = cfg.app;
    // Which machine this peer runs on. Two devices can share a name ("Viewer"),
    // and when they do the hostname is what tells them apart in a device list.
    if (!zcfg.headers.count("host")) {
        const std::string h = local_hostname();
        if (!h.empty()) zcfg.headers["host"] = h;
    }

    s->impl_->transport = std::make_unique<ZyreTransport>(name, std::move(zcfg));
    s->impl_->transport->connect();

    // The Node reuses the ZRE identity rather than minting its own, so the uuid
    // in a peer list is the same uuid you address a private message to.
    s->impl_->node = std::make_unique<Node>(name, s->impl_->transport->uuid(),
                                            s->impl_->transport.get(), cfg.compression);
    s->impl_->node->connect();

    // A network change rebuilds the ZRE node, and ZRE mints a new identity every
    // time. The Node has to adopt it: outgoing messages carry that uuid and
    // incoming `dst` is matched against it, so a stale one means every addressed
    // message to this device is dropped while broadcasts still arrive - a
    // half-working session that reports itself healthy.
    //
    // Raw pointers are safe only because Session::close() disconnects the
    // transport before releasing either object, which joins the thread this
    // fires on. See the note there; without it this is a use-after-free.
    // ── bridging ──
    // Only when asked, only when there is more than one usable network, and
    // never in gossip mode: gossip has no beacon and no per-network node, so
    // there is nothing to bridge between.
    const bool gossip = !cfg.gossip_bind.empty() || !cfg.gossip_connect.empty();
    if (cfg.allow_bridging && !gossip && s->impl_->transport->connected()) {
        const Interface primary = s->impl_->transport->chosen_interface();
        const std::string helper = find_bridge_helper(cfg.bridge_helper);
        for (const auto& iface : bridgeable(primary, cfg.allow_usb)) {
            auto link = std::make_unique<BridgeLink>();
            link->iface = iface.name;
            link->endpoint = "ipc:///tmp/uninet-bridge-" + std::to_string(::getpid()) +
                             "-" + iface.name;
            // Bind BEFORE starting the helper: a PAIR that connects to nothing
            // waits silently, and the first relayed message would be lost.
            link->sock = zsock_new_pair(("@" + link->endpoint).c_str());
            if (!link->sock) continue;

            const pid_t pid = ::fork();
            if (pid == 0) {
                ::execlp(helper.c_str(), helper.c_str(),
                         "--iface", iface.name.c_str(),
                         "--realm", cfg.realm.c_str(),
                         "--link", link->endpoint.c_str(),
                         "--name", (name + " (bridge)").c_str(),
                         (char*)nullptr);
                ::_exit(127);          // exec failed; the parent sees the socket stay quiet
            }
            if (pid < 0) { zsock_destroy(&link->sock); continue; }
            link->pid = pid;
            s->impl_->bridges.push_back(std::move(link));
        }
    }

    Node* node = s->impl_->node.get();
    ZyreTransport* transport = s->impl_->transport.get();
    Session* self = s.get();
    transport->on_reconnected([node, transport, self] {
        node->set_uuid(transport->uuid());
        // Keep the crash snapshot current. A report showing the network the
        // process was on three reconnects ago would point at the wrong thing.
        diagnostics_refresh();
    });
    // Local traffic goes out to the bridges too. A message this node RECEIVES
    // from its own network is forwarded; a message this node SENDS is forwarded
    // by publish(), because ZRE never delivers a node its own broadcast and it
    // would otherwise never reach the other networks at all.
    if (!s->impl_->bridges.empty()) s->start_relay();

    diagnostics_register(self);
    return s;
}

// Start the relay thread and the local fan-out subscription, once.
void Session::start_relay() {
    Impl* impl = impl_.get();
    if (impl->relaying.exchange(true)) return;   // already running

    impl->node->subscribe(">", [impl](const Envelope& env) {
        if (impl->seen_before(env.mid)) return;
        impl->fan_out(env.subject, env.data, env.mid, nullptr);
    });

    {
        impl->relay = std::thread([impl] {
            zpoller_t* poller = zpoller_new(nullptr);
            for (auto& b : impl->bridges) zpoller_add(poller, b->sock);
            while (impl->relaying.load()) {
                void* which = zpoller_wait(poller, 200);
                if (zpoller_terminated(poller)) break;
                if (!which) continue;
                BridgeLink* from = nullptr;
                for (auto& b : impl->bridges) if (b->sock == which) from = b.get();
                if (!from) continue;

                zmsg_t* m = zmsg_recv(which);
                if (!m) continue;
                char* subject = zmsg_popstr(m);
                char* mid = zmsg_popstr(m);
                zframe_t* body = zmsg_pop(m);
                if (subject && mid && body) {
                    const std::string id = mid;
                    if (!impl->seen_before(id)) {
                        bool ok = false;
                        Cbor data = decode(zframe_data(body), zframe_size(body), &ok);
                        if (ok) {
                            // Onto this node's own network, and on to the other
                            // bridges, carrying the SAME id so nothing treats it
                            // as new.
                            std::shared_lock<std::shared_timed_mutex> lk(impl->mu);
                            if (impl->node) impl->node->publish(subject, data, "", id);
                            lk.unlock();
                            impl->fan_out(subject, data, id, from);
                        }
                    }
                }
                freen(subject);
                freen(mid);
                zframe_destroy(&body);
                zmsg_destroy(&m);
            }
            zpoller_destroy(&poller);
        });
    }
}

bool Session::adopt_bridge(const std::string& label, const std::string& endpoint) {
    auto link = std::make_unique<BridgeLink>();
    link->iface = label;
    link->endpoint = endpoint;
    link->sock = zsock_new_pair(("@" + endpoint).c_str());
    if (!link->sock) return false;
    link->pid = -1;                 // not ours to stop; the caller owns it
    {
        std::unique_lock<std::shared_timed_mutex> lk(impl_->mu);
        impl_->bridges.push_back(std::move(link));
    }
    start_relay();
    return true;
}

bool Session::publish(const std::string& subject, Cbor data, const std::string& dst,
                      const std::string& mid) {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    if (!impl_->node) return false;
    // Mint the id here rather than inside Node, so the same one goes to the
    // local network AND to the bridges. Letting Node generate it would give the
    // bridged copies a different identity, and the loop guard would never fire.
    const std::string id = mid.empty() ? make_message_id() : mid;
    const bool ok = impl_->node->publish(subject, data, dst, id);
    // Addressed messages are not bridged: `dst` is a uuid on THIS network and
    // means nothing on another one, so forwarding it would send a private
    // message to a network that cannot act on it.
    if (dst.empty() && !impl_->bridges.empty()) {
        impl_->seen_before(id);           // do not relay our own message back
        impl_->fan_out(subject, data, id, nullptr);
    }
    return ok;
}

bool Session::publish_json(const std::string& subject, const std::string& json,
                           const std::string& dst) {
    bool ok = false;
    Cbor data = from_json(json, &ok);
    if (!ok) return false;   // malformed JSON is the caller's bug; say so, don't send
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    return impl_->node && impl_->node->publish(subject, std::move(data), dst);
}

void Session::subscribe(const std::string& subject, Node::DataHandler handler) {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    if (impl_->node) impl_->node->subscribe(subject, std::move(handler));
}

std::vector<Peer> Session::peers() const {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    return impl_->transport ? impl_->transport->peers() : std::vector<Peer>{};
}

void Session::on_peer_found(PeerCallback cb) {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    if (impl_->transport) impl_->transport->on_peer_found(std::move(cb));
}
void Session::on_peer_lost(PeerCallback cb) {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    if (impl_->transport) impl_->transport->on_peer_lost(std::move(cb));
}

bool Session::connected() const {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    return impl_->transport && impl_->transport->connected();
}
const std::string& Session::name() const { return impl_->name; }
std::string Session::uuid() const {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    return impl_->transport ? impl_->transport->uuid() : std::string();
}

Node& Session::node() {
    // Dereferencing after close() was silent undefined behaviour. The reference
    // is only as good as the caller's own discipline afterwards, which is why
    // the header says so.
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    if (!impl_->node) throw std::runtime_error("uninet: the session is closed");
    return *impl_->node;
}

ZyreTransport& Session::transport() {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    if (!impl_->transport) throw std::runtime_error("uninet: the session is closed");
    return *impl_->transport;
}

std::string Session::last_error() const {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    return impl_->transport ? impl_->transport->last_error() : "the session is closed";
}

std::string Session::describe() const {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    if (!impl_->transport) return "Closed.";
    if (!impl_->transport->connected()) {
        const std::string err = impl_->transport->last_error();
        return "Not on the network" + (err.empty() ? "." : ": " + err);
    }
    const size_t n = impl_->transport->peers().size();
    if (n == 0) return "On the network as \"" + impl_->name +
                       "\", no other devices yet.";
    return "On the network as \"" + impl_->name + "\": " + std::to_string(n) +
           (n == 1 ? " other device." : " other devices.");
}

}  // namespace uninet
