// UniNet: C ABI. A flat extern "C" surface over the C++ core, for C# (P/Invoke),
// Unity, and any other language with an FFI.
//
// The shape mirrors the C++ Session: join the network under a name, publish,
// subscribe, and ask who else is there. No address, no port, no broker.
//
//     uninet_session_t* s = uninet_session_join("Viewer", "viewer", "MyApp",
//                                               NULL, NULL, 0);
//     uninet_session_subscribe_json(s, "domain.>", on_message, NULL);
//     uninet_session_publish_json(s, "domain.D1", "{\"code\":\"update\"}", NULL);
//
// ── CONVENTIONS ───────────────────────────────────────────────────────────
//
// RETURN VALUES. Every function returning `int` returns UNINET_OK (0) on
// success and a negative UNINET_ERR_* on failure. Functions returning a pointer
// return NULL on failure. Either way, uninet_last_error() explains what went
// wrong. (v0.1 mixed 1=ok and 0=ok conventions between adjacent functions and
// documented neither.)
//
// STRINGS. Everything crossing this boundary is UTF-8, never the platform
// code page. Out-parameters are caller-provided buffers: pass a buffer and its
// size, get back the number of bytes written (excluding the NUL), or a negative
// error if it did not fit. Nothing returns a pointer the caller must free
// except the peer snapshot, which has an explicit free.
//
// THREADING. **Callbacks are invoked on UniNet's background network thread, not
// on the thread that registered them.** In Unity this means you must NOT touch
// the Unity API from a callback: marshal to the main thread first. The C#
// wrapper does this for you by default; a raw C consumer must do it itself.
// Callbacks must not block: the network loop is stalled while one runs.
//
// LIFETIME. A callback's `subject`, `src_uuid`, `json` and `data` pointers are
// valid only for the duration of the call. Copy anything you need to keep.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── status codes ──
#define UNINET_OK              0
#define UNINET_ERR_ARG        -1   // a required argument was NULL or out of range
#define UNINET_ERR_STATE      -2   // not connected, or the handle is unusable
#define UNINET_ERR_PARSE      -3   // the JSON or CBOR payload was malformed
#define UNINET_ERR_BUFFER     -4   // the caller's buffer was too small
#define UNINET_ERR_INTERNAL   -5   // an exception was caught at the boundary

typedef struct uninet_session uninet_session_t;
typedef struct uninet_peers   uninet_peers_t;

// ── callbacks ──
// A message whose payload is rendered as JSON text. Easiest to consume; costs a
// CBOR->JSON conversion per message.
typedef void (*uninet_json_cb)(const char* subject, const char* src_uuid,
                               const char* json, void* user);
// The same message with the payload as raw CBOR bytes, for a consumer that has
// its own CBOR library, or that wants to avoid the text conversion on a large
// mesh payload.
typedef void (*uninet_cbor_cb)(const char* subject, const char* src_uuid,
                               const uint8_t* data, size_t len, void* user);
// A device arriving or leaving.
typedef void (*uninet_peer_cb)(const char* uuid, const char* name, const char* address,
                               const char* role, const char* app, void* user);

// ── session ───────────────────────────────────────────────────────────────
// Join the network. Only `name` is required; pass NULL for any of the rest to
// take the default (realm "uninet", auto interface, port 5670). Never blocks
// long, and returns a handle even when the network is unavailable: check
// uninet_session_connected().
uninet_session_t* uninet_session_join(const char* name, const char* role, const char* app,
                                      const char* realm, const char* iface, int port);

// Same, for a link with no multicast: a USB-tethered device behind a port
// forward, a VPN, a routed network. One node passes `gossip_bind` to become the
// rendezvous; the others pass `gossip_connect` pointing at it. `endpoint` is
// this node's own data endpoint, and `advertised_endpoint` is what to tell peers
// to dial when that differs from what it binds. Any of them may be NULL.
uninet_session_t* uninet_session_join_ex(const char* name, const char* role, const char* app,
                                         const char* realm, const char* iface, int port,
                                         const char* gossip_bind, const char* gossip_connect,
                                         const char* endpoint,
                                         const char* advertised_endpoint);

// Leave the network and release the handle. Peers are told immediately rather
// than waiting for a timeout. Safe to call with NULL.
void uninet_session_free(uninet_session_t* session);

int uninet_session_connected(uninet_session_t* session);   // 1 connected, 0 not

// This device's address, for others to send it a private message. Writes UTF-8
// into `buf`; returns the length written, or a negative error.
int uninet_session_uuid(uninet_session_t* session, char* buf, size_t buflen);
// One plain-language sentence about the connection, for a status bar.
int uninet_session_describe(uninet_session_t* session, char* buf, size_t buflen);

// Send. `dst` NULL or empty broadcasts; otherwise it is a peer uuid and only
// that device receives the message.
int uninet_session_publish_json(uninet_session_t* session, const char* subject,
                                const char* json, const char* dst);
int uninet_session_publish_cbor(uninet_session_t* session, const char* subject,
                                const uint8_t* cbor, size_t len, const char* dst);

// Receive. `subject` is exact, or ends in ">" to match everything below it.
// The callback fires on the network thread (see THREADING above).
int uninet_session_subscribe_json(uninet_session_t* session, const char* subject,
                                  uninet_json_cb cb, void* user);
int uninet_session_subscribe_cbor(uninet_session_t* session, const char* subject,
                                  uninet_cbor_cb cb, void* user);

// Presence. on_peer_found also replays the devices already present, so
// registration order does not change what you see.
int uninet_session_on_peer_found(uninet_session_t* session, uninet_peer_cb cb, void* user);
int uninet_session_on_peer_lost(uninet_session_t* session, uninet_peer_cb cb, void* user);

// ── peer snapshot ─────────────────────────────────────────────────────────
// A point-in-time list. The returned strings stay valid until the snapshot is
// freed, which is what makes this safe to walk without copying as you go.
uninet_peers_t* uninet_session_peers(uninet_session_t* session);
int             uninet_peers_count(uninet_peers_t* peers);
const char*     uninet_peers_uuid(uninet_peers_t* peers, int index);
const char*     uninet_peers_name(uninet_peers_t* peers, int index);
const char*     uninet_peers_address(uninet_peers_t* peers, int index);
const char*     uninet_peers_role(uninet_peers_t* peers, int index);
const char*     uninet_peers_app(uninet_peers_t* peers, int index);
const char*     uninet_peers_header(uninet_peers_t* peers, int index, const char* key);
void            uninet_peers_free(uninet_peers_t* peers);

// ── data conversion ───────────────────────────────────────────────────────
// JSON <-> CBOR, so a consumer can work in whichever it prefers and still put
// identical bytes on the wire.
int uninet_json_to_cbor(const char* json, uint8_t* out, size_t outlen, size_t* written);
int uninet_cbor_to_json(const uint8_t* cbor, size_t len, char* out, size_t outlen);

// ── diagnostics ───────────────────────────────────────────────────────────
// Why the last call on THIS THREAD failed. Valid until the next failing call on
// the same thread. Never NULL.
const char* uninet_last_error(void);

uint16_t    uninet_protocol_version(void);
int         uninet_has_lz4(void);
const char* uninet_version(void);   // "zyre x.y.z / czmq ... / zmq ..."; static storage

#ifdef __cplusplus
}
#endif
