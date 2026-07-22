// UniNet — NATS transport. The production brokered backend, mirroring the bus the
// three ThermoNav peers share today (nats://10.0.0.10:4222 over WireGuard). Gated
// behind UNINET_NATS=ON (FetchContent cnats), like UniVox's optional CUDA tier.
#pragma once

#include "uninet/transport.h"

#ifdef UNINET_HAS_NATS
#include <nats/nats.h>
#endif

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace uninet {

class NatsTransport : public Transport {
public:
    explicit NatsTransport(std::string url = "nats://127.0.0.1:4222");
    ~NatsTransport() override;

    NatsTransport(const NatsTransport&) = delete;
    NatsTransport& operator=(const NatsTransport&) = delete;

    bool connect() override;
    void disconnect() override;
    bool connected() const override;
    bool publish(const std::string& subject, const uint8_t* data, size_t len) override;
    void subscribe(const std::string& subject, MessageHandler handler) override;
    void unsubscribe(const std::string& subject) override;
    std::string name() const override { return "nats"; }

#ifdef UNINET_HAS_NATS
private:
    // cnats delivers on its own thread; we route each subject's frames to its handler.
    struct Sub {
        MessageHandler handler;
#ifdef UNINET_HAS_NATS
        natsSubscription* sub = nullptr;
#endif
    };
    static void on_msg_(natsConnection* /*nc*/, natsSubscription* /*sub*/, natsMsg* msg, void* closure);

    std::string url_;
    natsConnection* conn_ = nullptr;
    mutable std::mutex mu_;
    std::map<std::string, Sub> subs_;
#else
private:
    std::string url_;
#endif
};

}  // namespace uninet
