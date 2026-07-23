// UniNet — C ABI implementation (loopback + NATS transports; text + raw-CBOR payloads).
#include "uninet/cabi.h"

#include "uninet/codec.h"
#include "uninet/loopback.h"
#include "uninet/node.h"
#include "uninet/nats_transport.h"

#include <memory>
#include <string>
#include <vector>

// Owns a loopback transport.
struct uninet_loopback {
    std::unique_ptr<uninet::LoopbackTransport> tp;
    explicit uninet_loopback(std::unique_ptr<uninet::LoopbackTransport> t) : tp(std::move(t)) {}
};

// Owns a NATS transport.
struct uninet_nats {
    std::unique_ptr<uninet::NatsTransport> tp;
    explicit uninet_nats(std::unique_ptr<uninet::NatsTransport> t) : tp(std::move(t)) {}
};

// Owns a node over a borrowed transport (loopback OR nats), plus kept-alive
// subscription thunks so the native side never calls into a moved/freed closure.
struct uninet_node {
    uninet::Node node;
    std::vector<std::function<void()>> keepalive;
    explicit uninet_node(const char* name, uninet::Transport* tp, int compression)
        : node(name ? name : "cabi", tp, uninet::Compression(uint8_t(compression))) {}
};

extern "C" {

// ── loopback ──
uninet_loopback_t* uninet_loopback_new(void) {
    auto tp = std::make_unique<uninet::LoopbackTransport>();
    tp->connect();
    return new uninet_loopback(std::move(tp));
}
void uninet_loopback_free(uninet_loopback_t* bus) { delete bus; }
int  uninet_loopback_connect(uninet_loopback_t*) { return 1; }

// ── NATS ──
uninet_nats_t* uninet_nats_new(const char* url) {
    return new uninet_nats(std::make_unique<uninet::NatsTransport>(url ? url : "nats://127.0.0.1:4222"));
}
void uninet_nats_free(uninet_nats_t* nats) { delete nats; }
int  uninet_nats_connect(uninet_nats_t* nats) { return nats && nats->tp->connect() ? 1 : 0; }

// ── node over either transport ──
uninet_node_t* uninet_node_new(const char* name, uninet_loopback_t* bus, int compression) {
    if (!bus) return nullptr;
    auto* n = new uninet_node(name, bus->tp.get(), compression);
    n->node.connect();
    return n;
}
uninet_node_t* uninet_node_new_nats(const char* name, uninet_nats_t* bus, int compression) {
    if (!bus) return nullptr;
    auto* n = new uninet_node(name, bus->tp.get(), compression);
    n->node.connect();
    return n;
}
void uninet_node_free(uninet_node_t* node) { delete node; }
int  uninet_node_connect(uninet_node_t* node) { return node && node->node.connect() ? 1 : 0; }

const char* uninet_node_uuid(uninet_node_t* node) {
    static thread_local std::string u;
    if (!node) return "";
    u = node->node.uuid();
    return u.c_str();
}

// ── text payload ──
int uninet_node_publish_text(uninet_node_t* node, const char* subject,
                             const char* dst_uuid, const char* text) {
    if (!node || !subject) return 0;
    uninet::Cbor data = uninet::Cbor::map()
                            .set("code", uninet::Cbor::text("message"))
                            .set("text", uninet::Cbor::text(text ? text : ""));
    node->node.publish(subject, std::move(data), dst_uuid ? dst_uuid : "");
    return 1;
}
int uninet_node_subscribe_text(uninet_node_t* node, const char* subject, uninet_text_cb cb, void* user) {
    if (!node || !subject || !cb) return -1;
    node->node.subscribe(subject, [cb, user](const uninet::Envelope& env) {
        const std::string& t = env.data.has("text") ? env.data["text"].as_text() : std::string{};
        cb(env.subject.c_str(), t.c_str(), user);
    });
    return 0;
}

// ── raw-CBOR payload (the envelope `data` field, exchanged as CBOR bytes) ──
int uninet_node_publish_cbor(uninet_node_t* node, const char* subject,
                             const char* dst_uuid, const uint8_t* cbor, size_t len) {
    if (!node || !subject || (!cbor && len)) return 0;
    bool ok = false;
    uninet::Cbor data = (len == 0) ? uninet::Cbor::null() : uninet::decode(cbor, len, &ok);
    if (len && !ok) return 0;
    node->node.publish(subject, std::move(data), dst_uuid ? dst_uuid : "");
    return 1;
}
int uninet_node_subscribe_cbor(uninet_node_t* node, const char* subject, uninet_cbor_cb cb, void* user) {
    if (!node || !subject || !cb) return -1;
    node->node.subscribe(subject, [cb, user](const uninet::Envelope& env) {
        uninet::Bytes b = uninet::encode(env.data);   // re-encode data -> CBOR bytes
        cb(env.subject.c_str(), env.src_uuid.c_str(), b.data(), b.size(), user);
    });
    return 0;
}

uint16_t uninet_protocol_version(void) { return uninet::CURRENT_PROTOCOL_VERSION; }
int uninet_has_lz4(void)  {
#ifdef UNINET_HAS_LZ4
    return 1;
#else
    return 0;
#endif
}
int uninet_has_nats(void) {
#ifdef UNINET_HAS_NATS
    return 1;
#else
    return 0;
#endif
}

}  // extern "C"
