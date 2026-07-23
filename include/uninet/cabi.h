// UniNet — C ABI for non-C++ bindings (C# via P/Invoke, etc.). A thin extern "C"
// surface over the C++ core. v0.1 exposes loopback + NATS transports and a Node at
// both the text-payload and raw-CBOR levels (raw-CBOR is how the MR headset, which
// uses PeterO.Cbor's CBORObject, exchanges arbitrary messages without a managed
// CBOR dependency in the binding).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uninet_loopback uninet_loopback_t;
typedef struct uninet_nats     uninet_nats_t;
typedef struct uninet_node     uninet_node_t;

// Text message: (subject, text payload, user). Strings NUL-terminated, valid only
// during the callback.
typedef void (*uninet_text_cb)(const char* subject, const char* text, void* user);
// Raw-CBOR message: (subject, src uuid, CBOR-encoded data bytes, len, user). The
// data pointer is valid only during the callback — copy if you need it after.
typedef void (*uninet_cbor_cb)(const char* subject, const char* src_uuid,
                               const uint8_t* data, size_t len, void* user);

// ── loopback transport ──
uninet_loopback_t* uninet_loopback_new(void);
void               uninet_loopback_free(uninet_loopback_t* bus);
int                uninet_loopback_connect(uninet_loopback_t* bus);   // always 1

// ── NATS transport (the production brokered backend; needs a NATS build) ──
uninet_nats_t* uninet_nats_new(const char* url);                      // e.g. "nats://10.0.0.10:4222"
void           uninet_nats_free(uninet_nats_t* nats);
int            uninet_nats_connect(uninet_nats_t* nats);              // 1 on success

// ── node (over either transport; compression: 0=None,1=Zlib,2=Lz4) ──
// The node borrows its transport (keep it alive until the node is freed).
uninet_node_t* uninet_node_new(const char* name, uninet_loopback_t* bus, int compression);
uninet_node_t* uninet_node_new_nats(const char* name, uninet_nats_t* bus, int compression);
void           uninet_node_free(uninet_node_t* node);
int            uninet_node_connect(uninet_node_t* node);
const char*    uninet_node_uuid(uninet_node_t* node);                  // borrowed until free

// Publish/subscribe a text payload.
int  uninet_node_publish_text(uninet_node_t* node, const char* subject,
                              const char* dst_uuid, const char* text);
int  uninet_node_subscribe_text(uninet_node_t* node, const char* subject,
                                uninet_text_cb cb, void* user);

// Publish/subscribe a raw CBOR payload (the envelope `data` field). publish_cbor
// decodes `cbor` into UniNet's Cbor and frames it; subscribe_cbor re-encodes the
// received `data` to CBOR bytes and hands them to the callback. Lets a binding
// that already has a CBOR lib (e.g. PeterO.Cbor on the MR headset) keep using it.
int  uninet_node_publish_cbor(uninet_node_t* node, const char* subject,
                              const char* dst_uuid, const uint8_t* cbor, size_t len);
int  uninet_node_subscribe_cbor(uninet_node_t* node, const char* subject,
                                uninet_cbor_cb cb, void* user);

// Library version + the build's compression/transport backends.
uint16_t uninet_protocol_version(void);
int      uninet_has_lz4(void);   // 1 if compiled with liblz4
int      uninet_has_nats(void);  // 1 if compiled with cnats

#ifdef __cplusplus
}
#endif
