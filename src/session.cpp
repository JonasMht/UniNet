// UniNet: Session implementation. See include/uninet/session.h.
#include "uninet/session.h"

#include "uninet/json.h"

#include <memory>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

namespace uninet {

struct Session::Impl {
    std::string name;
    std::unique_ptr<ZyreTransport> transport;
    std::unique_ptr<Node>          node;

    // close() resets these while another thread may be inside publish() or
    // peers(). The null checks alone are a time-of-check/time-of-use race:
    // AddressSanitizer showed a heap-use-after-free with a publisher thread
    // reading Node after close() had freed it. close() takes this exclusively;
    // every accessor takes it shared, so readers still run concurrently.
    mutable std::shared_timed_mutex mu;
};

Session::Session() : impl_(new Impl()) {}

Session::~Session() { close(); }

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
    return s;
}

bool Session::publish(const std::string& subject, Cbor data, const std::string& dst) {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    return impl_->node && impl_->node->publish(subject, std::move(data), dst);
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
const std::string& Session::uuid() const {
    std::shared_lock<std::shared_timed_mutex> lk(impl_->mu);
    static const std::string kNone;
    return impl_->transport ? impl_->transport->uuid() : kNone;
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
