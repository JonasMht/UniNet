// UniNet — NATS transport. The production brokered backend, mirroring the bus the
// three ThermoNav peers share today (nats://…:4222). Gated behind UNINET_NATS=ON
// (FetchContent cnats), like UniVox's optional CUDA tier.
//
// PImpl: the header layout is identical whether or not the consumer is built with
// UNINET_HAS_NATS, so a NATS-enabled libuninet can be linked from code compiled
// without the define (the Python extension, the future ThermoNav consumers) without
// an ABI mismatch. When UNINET_NATS=OFF the methods are link-time stubs (connect()
// returns false) — NatsTransport exists everywhere, works where built.
#pragma once

#include "uninet/transport.h"

#include <memory>
#include <string>

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
    // Block until the broker has acked all buffered publishes (pace a burst so a
    // slow consumer isn't dropped). No-op for non-NATS transports.
    void flush();
    std::string name() const override { return "nats"; }

private:
    struct Impl;                       // cnats state, defined in nats_transport.cpp
    std::string url_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace uninet
