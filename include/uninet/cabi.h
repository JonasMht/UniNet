// UniNet — C ABI for non-C++ bindings (C# via P/Invoke, etc.). A thin extern "C"
// surface over the C++ core, mirroring how UniVox could expose a C ABI. v0.1
// exposes the loopback transport + Node at the text-payload level (enough to demo
// end-to-end pub/sub from C# without a managed CBOR lib). Raw-CBOR and NATS-backed
// variants are staged.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct uninet_loopback uninet_loopback_t;
typedef struct uninet_node    uninet_node_t;

// A message delivered to a subscriber: (subject, text payload, user pointer).
// Strings are NUL-terminated and valid only for the duration of the callback.
typedef void (*uninet_text_cb)(const char* subject, const char* text, void* user);

// ── loopback transport ──
uninet_loopback_t* uninet_loopback_new(void);
void               uninet_loopback_free(uninet_loopback_t* bus);
int                uninet_loopback_connect(uninet_loopback_t* bus);   // always 1

// ── node ──
// compression: 0=None, 1=Zlib, 2=Lz4. The node borrows `bus` (keep it alive).
uninet_node_t* uninet_node_new(const char* name, uninet_loopback_t* bus, int compression);
void           uninet_node_free(uninet_node_t* node);
int            uninet_node_connect(uninet_node_t* node);
const char*    uninet_node_uuid(uninet_node_t* node);    // borrowed, valid until free

// Publish a text payload on `subject`, optionally targeted at `dst_uuid` ("").
int  uninet_node_publish_text(uninet_node_t* node, const char* subject,
                              const char* dst_uuid, const char* text);
// Subscribe to `subject` (exact or ">" wildcard). Returns a subscription id >=0.
int  uninet_node_subscribe_text(uninet_node_t* node, const char* subject,
                                uninet_text_cb cb, void* user);

// Library version + the build's compression backends (for the binding to report).
uint16_t uninet_protocol_version(void);
int      uninet_has_lz4(void);   // 1 if compiled with liblz4
int      uninet_has_nats(void);  // 1 if compiled with cnats

#ifdef __cplusplus
}
#endif
