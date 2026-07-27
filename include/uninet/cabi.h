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

// ── extra configuration ───────────────────────────────────────────────────
// Advertise a key/value to every peer, readable through uninet_peers_header.
// Must be called BEFORE the session joins, so it is set on a config handle
// rather than a session. Create one, set what you need, pass it to
// uninet_session_join_cfg, then free it.
typedef struct uninet_config uninet_config_t;

uninet_config_t* uninet_config_new(void);
void             uninet_config_free(uninet_config_t* cfg);
int uninet_config_set_header(uninet_config_t* cfg, const char* key, const char* value);
// 0=None, 1=Zlib, 2=Lz4. Anything else is rejected.
int uninet_config_set_compression(uninet_config_t* cfg, int compression);
int uninet_config_set_realm(uninet_config_t* cfg, const char* realm);
int uninet_config_set_role(uninet_config_t* cfg, const char* role);
int uninet_config_set_app(uninet_config_t* cfg, const char* app);
int uninet_config_set_interface(uninet_config_t* cfg, const char* iface);
int uninet_config_set_port(uninet_config_t* cfg, int port);
int uninet_config_set_gossip(uninet_config_t* cfg, const char* bind, const char* connect,
                             const char* endpoint, const char* advertised);

// Join with everything the C++ SessionConfig can express. `cfg` may be NULL,
// which is then identical to uninet_session_join(name, ...).
uninet_session_t* uninet_session_join_cfg(const char* name, uninet_config_t* cfg);

// ── session lifetime ──────────────────────────────────────────────────────
// Leave the network without destroying the handle. Idempotent. Useful when the
// shutdown order matters: a garbage-collected host may finalize objects after
// the ZeroMQ context is gone, and closing a socket after that aborts inside
// czmq. Call this while you still control the ordering, then free later.
void uninet_session_close(uninet_session_t* session);
int  uninet_session_open(uninet_session_t* session);   // 1 until close() is called

// ── large payloads: files, volumes, meshes ────────────────────────────────
// publish() sends a message that must fit in memory whole on both ends. A blob
// is chunked, streamed and reassembled, with progress at both ends, and carries
// arbitrary metadata alongside the bytes.
typedef struct uninet_blob uninet_blob_t;

// Fired once per completed transfer. `meta` is the sender's metadata rendered as
// JSON ("null" when none). `data`/`len` are valid only during the call.
typedef void (*uninet_blob_cb)(const char* id, const char* name, const char* src,
                               const char* meta_json, const uint8_t* data,
                               size_t len, void* user);
// Fired as chunks arrive. `done` of `total` bytes.
typedef void (*uninet_blob_progress_cb)(const char* id, const char* name,
                                        size_t done, size_t total, void* user);
// Fired when a transfer is abandoned (the sender vanished, or it stalled).
typedef void (*uninet_blob_failed_cb)(const char* id, const char* name,
                                      const char* reason, void* user);

// `subject` is a base subject; the transfer runs beneath it, so it never
// collides with your own messages on the same prefix. The session must outlive
// the blob.
uninet_blob_t* uninet_blob_new(uninet_session_t* session, const char* subject);
void           uninet_blob_free(uninet_blob_t* blob);

// Send bytes, or a file. `meta_json` may be NULL. `dst` NULL or empty broadcasts;
// otherwise only that peer receives it. Writes the transfer id into `id_buf` and
// returns its length, or a negative error. An empty id means the transfer could
// not start.
int uninet_blob_send(uninet_blob_t* blob, const char* name,
                     const uint8_t* data, size_t len, const char* meta_json,
                     const char* dst, char* id_buf, size_t id_buflen);
int uninet_blob_send_file(uninet_blob_t* blob, const char* path,
                          const char* meta_json, const char* dst,
                          char* id_buf, size_t id_buflen);

int uninet_blob_on_received(uninet_blob_t* blob, uninet_blob_cb cb, void* user);
int uninet_blob_on_progress(uninet_blob_t* blob, uninet_blob_progress_cb cb, void* user);
int uninet_blob_on_failed(uninet_blob_t* blob, uninet_blob_failed_cb cb, void* user);
int uninet_blob_incoming_count(uninet_blob_t* blob);

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
// 1 when the peer advertised `key` at all. Without this an absent header and an
// empty one both read as "".
int             uninet_peers_has_header(uninet_peers_t* peers, int index, const char* key);
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

// A snapshot of what UniNet is doing: version, compression tiers, every network
// on this machine and which one discovery chose, and every live session with
// its realm, identity, peer count and reconnect count. Writes UTF-8 into `buf`
// and returns the length, or UNINET_ERR_BUFFER when it does not fit (call with
// a NULL buffer to ask how much is needed).
//
// This is what to put behind a "copy diagnostics" button. Almost every "it
// cannot see the other device" report is answered by the network line.
int uninet_diagnostics(char* buf, size_t buflen);

// Write a crash report to `path` if the process dies on a fatal signal.
// Returns UNINET_OK, or UNINET_ERR_STATE if the handlers could not be
// installed. Off unless called: a library has no business taking over an
// application's fatal signal handling uninvited, and any existing handler is
// chained to rather than replaced.
int  uninet_enable_crash_log(const char* path);
void uninet_disable_crash_log(void);

#ifdef __cplusplus
}
#endif
