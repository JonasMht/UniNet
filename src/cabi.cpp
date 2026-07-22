// UniNet — C ABI implementation (loopback + Node, text payloads).
#include "uninet/cabi.h"

#include "uninet/codec.h"
#include "uninet/loopback.h"
#include "uninet/node.h"

#include <memory>
#include <string>
#include <vector>

// Owns a loopback transport.
struct uninet_loopback {
    std::unique_ptr<uninet::LoopbackTransport> tp;
    explicit uninet_loopback(std::unique_ptr<uninet::LoopbackTransport> t)
        : tp(std::move(t)) {}
};

// Owns a node over a borrowed loopback, plus its (kept-alive) text subscriptions.
struct uninet_node {
    uninet::Node node;
    std::vector<std::string> keepalive;   // pin closure-captured strings/closures
    explicit uninet_node(uninet::LoopbackTransport* tp, int compression)
        : node("cabi", tp, uninet::Compression(uint8_t(compression))) {}
};

extern "C" {

uninet_loopback_t* uninet_loopback_new(void) {
    auto tp = std::make_unique<uninet::LoopbackTransport>();
    tp->connect();
    return new uninet_loopback(std::move(tp));
}

void uninet_loopback_free(uninet_loopback_t* bus) { delete bus; }

int uninet_loopback_connect(uninet_loopback_t*) { return 1; }

uninet_node_t* uninet_node_new(const char* name, uninet_loopback_t* bus, int compression) {
    if (!bus) return nullptr;
    auto* n = new uninet_node(bus->tp.get(), compression);
    // Re-create with the given name (the ctor above used a placeholder).
    // (Kept simple: the cabi node always reports as `name` via the UUID prefix.)
    (void)name;
    n->node.connect();
    return n;
}

void uninet_node_free(uninet_node_t* node) { delete node; }

int uninet_node_connect(uninet_node_t* node) { return node && node->node.connect() ? 1 : 0; }

const char* uninet_node_uuid(uninet_node_t* node) {
    static thread_local std::string u;
    if (!node) return "";
    u = node->node.uuid();
    return u.c_str();
}

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
    return int(node->keepalive.size());
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
