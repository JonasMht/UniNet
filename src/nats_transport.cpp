// UniNet — NATS transport implementation (gated by UNINET_HAS_NATS).
#include "uninet/nats_transport.h"

#ifdef UNINET_HAS_NATS

namespace uninet {

NatsTransport::NatsTransport(std::string url) : url_(std::move(url)) {}

NatsTransport::~NatsTransport() {
    disconnect();
    nats_Close();
}

bool NatsTransport::connect() {
    if (conn_) return true;
    natsOptions* opts = nullptr;
    if (natsOptions_Create(&opts) != NATS_OK) return false;
    natsOptions_SetURL(opts, url_.c_str());
    natsOptions_SetAllowReconnect(opts, true);
    natsOptions_SetMaxReconnect(opts, -1);   // unlimited
    natsStatus s = natsConnection_Connect(&conn_, opts);
    natsOptions_Destroy(opts);
    return s == NATS_OK;
}

void NatsTransport::disconnect() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [k, sub] : subs_)
            if (sub.sub) { natsSubscription_Unsubscribe(sub.sub); natsSubscription_Destroy(sub.sub); sub.sub = nullptr; }
        subs_.clear();
    }
    if (conn_) { natsConnection_Destroy(conn_); conn_ = nullptr; }
}

bool NatsTransport::connected() const {
    return conn_ && natsConnection_Status(conn_) == CONNECTED;
}

bool NatsTransport::publish(const std::string& subject, const uint8_t* data, size_t len) {
    if (!connected()) return false;
    return natsConnection_Publish(conn_, subject.c_str(), data, len) == NATS_OK;
}

void NatsTransport::on_msg_(natsConnection*, natsSubscription*, natsMsg* msg, void* closure) {
    auto* h = static_cast<MessageHandler*>(closure);
    const char* subj = natsMsg_GetSubject(msg);
    const char* data = natsMsg_GetData(msg);
    int len = natsMsg_GetDataLength(msg);
    if (h && *h && data && len > 0) {
        (*h)(subj ? subj : "", uninet::Bytes(reinterpret_cast<const uint8_t*>(data),
                                             reinterpret_cast<const uint8_t*>(data) + len));
    }
    natsMsg_Destroy(msg);
}

void NatsTransport::subscribe(const std::string& subject, MessageHandler handler) {
    if (!connected()) return;
    std::lock_guard<std::mutex> lk(mu_);
    Sub& s = subs_[subject];
    if (s.sub) { natsSubscription_Unsubscribe(s.sub); natsSubscription_Destroy(s.sub); }
    s.handler = std::move(handler);
    // The closure points into the map entry; stable for the map's lifetime.
    natsSubscription* sub = nullptr;
    natsConnection_Subscribe(&sub, conn_, subject.c_str(), &NatsTransport::on_msg_, &s.handler);
    s.sub = sub;
}

void NatsTransport::unsubscribe(const std::string& subject) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = subs_.find(subject);
    if (it != subs_.end()) {
        if (it->second.sub) { natsSubscription_Unsubscribe(it->second.sub); natsSubscription_Destroy(it->second.sub); }
        subs_.erase(it);
    }
}

}  // namespace uninet

#endif  // UNINET_HAS_NATS
