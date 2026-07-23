// UniNet — NATS transport implementation (PImpl; gated by UNINET_HAS_NATS).
#include "uninet/nats_transport.h"

#ifdef UNINET_HAS_NATS
#include <nats/nats.h>
#endif

#include <map>
#include <mutex>

namespace uninet {

#ifdef UNINET_HAS_NATS

struct NatsTransport::Impl {
    natsConnection* conn = nullptr;
    mutable std::mutex mu;
    struct Sub { MessageHandler handler; natsSubscription* sub = nullptr; };
    std::map<std::string, Sub> subs;

    // cnats delivers on its own thread; route each subject's frame to its handler.
    static void on_msg(natsConnection*, natsSubscription*, natsMsg* msg, void* closure) {
        auto* h = static_cast<MessageHandler*>(closure);
        const char* subj = natsMsg_GetSubject(msg);
        const char* data = natsMsg_GetData(msg);
        int len = natsMsg_GetDataLength(msg);
        if (h && *h && data && len > 0)
            (*h)(subj ? subj : "", Bytes(reinterpret_cast<const uint8_t*>(data),
                                         reinterpret_cast<const uint8_t*>(data) + len));
        natsMsg_Destroy(msg);
    }
};

NatsTransport::NatsTransport(std::string url) : url_(std::move(url)), impl_(std::make_unique<Impl>()) {}
NatsTransport::~NatsTransport() { disconnect(); }

bool NatsTransport::connect() {
    if (impl_->conn) return true;
    natsOptions* opts = nullptr;
    if (natsOptions_Create(&opts) != NATS_OK) return false;
    natsOptions_SetURL(opts, url_.c_str());
    natsOptions_SetAllowReconnect(opts, true);
    natsOptions_SetMaxReconnect(opts, -1);   // unlimited
    natsStatus s = natsConnection_Connect(&impl_->conn, opts);
    natsOptions_Destroy(opts);
    return s == NATS_OK;
}

void NatsTransport::disconnect() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        for (auto& [k, sub] : impl_->subs) {
            if (sub.sub) {
                natsSubscription_Unsubscribe(sub.sub);
                natsSubscription_Destroy(sub.sub);
                sub.sub = nullptr;
                sub.handler = nullptr;   // release closures before the map goes
            }
        }
        impl_->subs.clear();
    }
    if (impl_->conn) { natsConnection_Destroy(impl_->conn); impl_->conn = nullptr; }
}

bool NatsTransport::connected() const {
    return impl_->conn && natsConnection_Status(impl_->conn) == NATS_CONN_STATUS_CONNECTED;
}

bool NatsTransport::publish(const std::string& subject, const uint8_t* data, size_t len) {
    if (!connected()) return false;
    return natsConnection_Publish(impl_->conn, subject.c_str(), data, len) == NATS_OK;
}

void NatsTransport::subscribe(const std::string& subject, MessageHandler handler) {
    if (!connected()) return;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto& s = impl_->subs[subject];
    if (s.sub) { natsSubscription_Unsubscribe(s.sub); natsSubscription_Destroy(s.sub); s.sub = nullptr; }
    s.handler = std::move(handler);
    natsSubscription* sub = nullptr;
    natsConnection_Subscribe(&sub, impl_->conn, subject.c_str(), &Impl::on_msg, &s.handler);
    s.sub = sub;
}

void NatsTransport::unsubscribe(const std::string& subject) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->subs.find(subject);
    if (it != impl_->subs.end()) {
        if (it->second.sub) {
            natsSubscription_Unsubscribe(it->second.sub);
            natsSubscription_Destroy(it->second.sub);
        }
        impl_->subs.erase(it);
    }
}

#else  // !UNINET_HAS_NATS — stubs so the symbol set is stable either way.

struct NatsTransport::Impl {};
NatsTransport::NatsTransport(std::string url) : url_(std::move(url)), impl_(std::make_unique<Impl>()) {}
NatsTransport::~NatsTransport() = default;
bool NatsTransport::connect()                                  { return false; }
void NatsTransport::disconnect()                               {}
bool NatsTransport::connected() const                          { return false; }
bool NatsTransport::publish(const std::string&, const uint8_t*, size_t) { return false; }
void  NatsTransport::subscribe(const std::string&, MessageHandler)      {}
void  NatsTransport::unsubscribe(const std::string&)                    {}

#endif  // UNINET_HAS_NATS

}  // namespace uninet
