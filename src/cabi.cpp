// UniNet: C ABI implementation. See include/uninet/cabi.h.
//
// Every extern "C" function is wrapped in a catch-all. A C++ exception that
// escapes through a C frame is undefined behaviour, and the callers on the far
// side: C#, Unity, plain C: cannot unwind it. v0.1 had no guards at all, so a
// std::bad_alloc on a large payload killed the process.
#include "uninet/cabi.h"

#include "uninet/blob.h"
#include "uninet/json.h"
#include "uninet/session.h"

#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <new>
#include <string>
#include <vector>

using namespace uninet;

namespace {

// Per-thread, so two threads failing at once do not overwrite each other's
// explanation. The header documents this lifetime.
thread_local std::string g_error;

void set_error(const std::string& e) { g_error = e; }

// Copy into a caller-provided buffer. Returns the bytes written, or
// UNINET_ERR_BUFFER when it would not fit, never a truncated string the caller
// might mistake for the whole value.
int copy_out(const std::string& s, char* buf, size_t buflen) {
    if (!buf || buflen == 0) {
        set_error("output buffer is null or empty");
        return UNINET_ERR_ARG;
    }
    if (s.size() + 1 > buflen) {
        set_error("output buffer needs " + std::to_string(s.size() + 1) + " bytes");
        return UNINET_ERR_BUFFER;
    }
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = '\0';
    return int(s.size());
}

const char* safe(const char* s) { return s ? s : ""; }

}  // namespace

// The handle types are the C++ objects plus whatever the ABI keeps alive for
// them. Subscriptions capture a C function pointer and a void*, so nothing needs
// pinning here, but the Session must outlive every callback it holds, and
// freeing the handle is what stops them.
struct uninet_session {
    std::unique_ptr<Session> session;
};

struct uninet_peers {
    std::vector<Peer> items;
    // header_of() fills this lazily, so two threads reading the same snapshot
    // would race on the map. Nothing in the header promised single-threaded use.
    std::mutex fields_mu;
    // role/app/header return computed strings, so they need storage that
    // outlives the call. One shared scratch buffer would alias, a caller
    // holding the result of role() and then calling app() would find its first
    // pointer now showing the second value. Each (index, key) therefore gets its
    // own entry, and std::map guarantees the value's address never moves.
    std::map<std::string, std::string> fields;
};

// ── session ───────────────────────────────────────────────────────────────

extern "C" uninet_session_t* uninet_session_join_ex(
        const char* name, const char* role, const char* app, const char* realm,
        const char* iface, int port, const char* gossip_bind,
        const char* gossip_connect, const char* endpoint,
        const char* advertised_endpoint) {
    try {
        if (!name || !*name) { set_error("name is required"); return nullptr; }
        SessionConfig cfg;
        cfg.role = safe(role);
        cfg.app  = safe(app);
        if (realm && *realm) cfg.realm = realm;
        if (iface && *iface) cfg.iface = iface;
        if (port > 0)        cfg.port = port;
        cfg.gossip_bind         = safe(gossip_bind);
        cfg.gossip_connect      = safe(gossip_connect);
        cfg.endpoint            = safe(endpoint);
        cfg.advertised_endpoint = safe(advertised_endpoint);

        auto handle = std::unique_ptr<uninet_session>(new uninet_session());
        handle->session = Session::join(name, std::move(cfg));
        return handle.release();
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    } catch (...) {
        set_error("unknown error while joining the network");
        return nullptr;
    }
}

extern "C" uninet_session_t* uninet_session_join(const char* name, const char* role,
                                                 const char* app, const char* realm,
                                                 const char* iface, int port) {
    return uninet_session_join_ex(name, role, app, realm, iface, port,
                                  nullptr, nullptr, nullptr, nullptr);
}

extern "C" void uninet_session_free(uninet_session_t* session) {
    try { delete session; } catch (...) {}
}

extern "C" int uninet_session_connected(uninet_session_t* session) {
    try {
        return (session && session->session && session->session->connected()) ? 1 : 0;
    } catch (...) { return 0; }
}

extern "C" int uninet_session_uuid(uninet_session_t* session, char* buf, size_t buflen) {
    try {
        if (!session || !session->session) { set_error("null session"); return UNINET_ERR_ARG; }
        return copy_out(session->session->uuid(), buf, buflen);
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_session_describe(uninet_session_t* session, char* buf, size_t buflen) {
    try {
        if (!session || !session->session) { set_error("null session"); return UNINET_ERR_ARG; }
        return copy_out(session->session->describe(), buf, buflen);
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_session_publish_json(uninet_session_t* session, const char* subject,
                                           const char* json, const char* dst) {
    try {
        if (!session || !session->session || !subject || !json) {
            set_error("null session, subject or payload");
            return UNINET_ERR_ARG;
        }
        // Parse first, so a send failure is never reported as a parse failure.
        // Both used to return UNINET_ERR_PARSE, so a caller could not tell a
        // malformed payload from being offline.
        bool ok = false;
        Cbor data = from_json(json, &ok);
        if (!ok) { set_error("payload is not valid JSON"); return UNINET_ERR_PARSE; }
        if (!session->session->publish(subject, std::move(data), safe(dst))) {
            set_error("not on the network, or the transport refused the message");
            return UNINET_ERR_STATE;
        }
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_session_publish_cbor(uninet_session_t* session, const char* subject,
                                           const uint8_t* cbor, size_t len, const char* dst) {
    try {
        if (!session || !session->session || !subject || (!cbor && len)) {
            set_error("null session, subject or payload");
            return UNINET_ERR_ARG;
        }
        bool ok = false;
        Cbor data = decode(cbor, len, &ok);
        if (!ok) { set_error("payload is not valid CBOR"); return UNINET_ERR_PARSE; }
        // The bool was discarded here, so this function could never fail: a
        // message published while offline reported UNINET_OK and vanished.
        if (!session->session->publish(subject, std::move(data), safe(dst))) {
            set_error("not on the network, or the transport refused the message");
            return UNINET_ERR_STATE;
        }
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_session_subscribe_json(uninet_session_t* session, const char* subject,
                                             uninet_json_cb cb, void* user) {
    try {
        if (!session || !session->session || !subject || !cb) {
            set_error("null session, subject or callback");
            return UNINET_ERR_ARG;
        }
        // Subscribing on a closed session is a no-op inside Session; reporting
        // OK for it meant the caller believed they were receiving.
        if (!session->session->open()) {
            set_error("the session is closed");
            return UNINET_ERR_STATE;
        }
        session->session->subscribe(subject, [cb, user](const Envelope& env) {
            // A managed exception crossing back over a reverse P/Invoke must not
            // unwind through the network loop.
            try {
                const std::string json = to_json(env.data);
                cb(env.subject.c_str(), env.src_uuid.c_str(), json.c_str(), user);
            } catch (...) {}
        });
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_session_subscribe_cbor(uninet_session_t* session, const char* subject,
                                             uninet_cbor_cb cb, void* user) {
    try {
        if (!session || !session->session || !subject || !cb) {
            set_error("null session, subject or callback");
            return UNINET_ERR_ARG;
        }
        if (!session->session->open()) {
            set_error("the session is closed");
            return UNINET_ERR_STATE;
        }
        session->session->subscribe(subject, [cb, user](const Envelope& env) {
            try {
                const Bytes bytes = encode(env.data);
                cb(env.subject.c_str(), env.src_uuid.c_str(), bytes.data(), bytes.size(), user);
            } catch (...) {}
        });
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

namespace {

// Both presence callbacks marshal a Peer the same way.
Session::PeerCallback wrap_peer_cb(uninet_peer_cb cb, void* user) {
    return [cb, user](const Peer& p) {
        try {
            const std::string role = p.role(), app = p.app();
            cb(p.uuid.c_str(), p.name.c_str(), p.address.c_str(),
               role.c_str(), app.c_str(), user);
        } catch (...) {}
    };
}

}  // namespace

extern "C" int uninet_session_on_peer_found(uninet_session_t* session,
                                            uninet_peer_cb cb, void* user) {
    try {
        if (!session || !session->session || !cb) {
            set_error("null argument");
            return UNINET_ERR_ARG;
        }
        if (!session->session->open()) {
            set_error("the session is closed");
            return UNINET_ERR_STATE;
        }
        session->session->on_peer_found(wrap_peer_cb(cb, user));
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_session_on_peer_lost(uninet_session_t* session,
                                           uninet_peer_cb cb, void* user) {
    try {
        if (!session || !session->session || !cb) {
            set_error("null argument");
            return UNINET_ERR_ARG;
        }
        if (!session->session->open()) {
            set_error("the session is closed");
            return UNINET_ERR_STATE;
        }
        session->session->on_peer_lost(wrap_peer_cb(cb, user));
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

// ── extra configuration ───────────────────────────────────────────────────

struct uninet_config {
    SessionConfig cfg;
};

extern "C" uninet_config_t* uninet_config_new(void) {
    try { return new uninet_config(); }
    catch (...) { set_error("out of memory"); return nullptr; }
}

extern "C" void uninet_config_free(uninet_config_t* cfg) {
    try { delete cfg; } catch (...) {}
}

namespace {
// Every setter shares the same null check and failure code.
int with_cfg(uninet_config_t* cfg, const std::function<void(SessionConfig&)>& fn) {
    try {
        if (!cfg) { set_error("null config"); return UNINET_ERR_ARG; }
        fn(cfg->cfg);
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}
}  // namespace

extern "C" int uninet_config_set_header(uninet_config_t* cfg, const char* key,
                                        const char* value) {
    if (!key || !*key) { set_error("header key is required"); return UNINET_ERR_ARG; }
    return with_cfg(cfg, [&](SessionConfig& c) { c.headers[key] = safe(value); });
}

extern "C" int uninet_config_set_compression(uninet_config_t* cfg, int compression) {
    // Validated rather than cast: an out-of-range value produced an enum with no
    // enumerator, and the codec then silently emitted an empty payload.
    if (compression < 0 || compression > 2) {
        set_error("compression must be 0 (none), 1 (zlib) or 2 (lz4)");
        return UNINET_ERR_ARG;
    }
    // Range alone was not enough. Asking for LZ4 on a build without liblz4 let
    // compress_into fall through to an empty buffer, so every publish shipped a
    // valid header with a zero-length payload, reported success, and was dropped
    // by the receiver. Every message vanished.
#ifndef UNINET_HAS_LZ4
    if (compression == 2) {
        set_error("this build has no liblz4; use 0 (none) or 1 (zlib)");
        return UNINET_ERR_ARG;
    }
#endif
    return with_cfg(cfg, [&](SessionConfig& c) {
        c.compression = static_cast<Compression>(compression);
    });
}

extern "C" int uninet_config_set_realm(uninet_config_t* cfg, const char* realm) {
    // An empty realm used to be accepted and silently leave the default in
    // place. The realm is what keeps a development machine out of a live
    // session, so failing quietly there is an isolation failure.
    if (!realm || !*realm) { set_error("realm must not be empty"); return UNINET_ERR_ARG; }
    return with_cfg(cfg, [&](SessionConfig& c) { c.realm = realm; });
}
extern "C" int uninet_config_set_role(uninet_config_t* cfg, const char* role) {
    return with_cfg(cfg, [&](SessionConfig& c) { c.role = safe(role); });
}
extern "C" int uninet_config_set_app(uninet_config_t* cfg, const char* app) {
    return with_cfg(cfg, [&](SessionConfig& c) { c.app = safe(app); });
}
extern "C" int uninet_config_set_interface(uninet_config_t* cfg, const char* iface) {
    return with_cfg(cfg, [&](SessionConfig& c) { c.iface = safe(iface); });
}
extern "C" int uninet_config_set_port(uninet_config_t* cfg, int port) {
    if (port < 0 || port > 65535) { set_error("port out of range"); return UNINET_ERR_ARG; }
    return with_cfg(cfg, [&](SessionConfig& c) { if (port > 0) c.port = port; });
}
extern "C" int uninet_config_set_gossip(uninet_config_t* cfg, const char* bind,
                                        const char* connect, const char* endpoint,
                                        const char* advertised) {
    return with_cfg(cfg, [&](SessionConfig& c) {
        c.gossip_bind         = safe(bind);
        c.gossip_connect      = safe(connect);
        c.endpoint            = safe(endpoint);
        c.advertised_endpoint = safe(advertised);
    });
}

extern "C" uninet_session_t* uninet_session_join_cfg(const char* name,
                                                     uninet_config_t* cfg) {
    try {
        if (!name || !*name) { set_error("name is required"); return nullptr; }
        auto handle = std::unique_ptr<uninet_session>(new uninet_session());
        handle->session = Session::join(name, cfg ? cfg->cfg : SessionConfig{});
        return handle.release();
    } catch (const std::exception& e) { set_error(e.what()); return nullptr; }
      catch (...) { set_error("unknown error while joining the network"); return nullptr; }
}

// ── session lifetime ──────────────────────────────────────────────────────

extern "C" void uninet_session_close(uninet_session_t* session) {
    try { if (session && session->session) session->session->close(); } catch (...) {}
}

extern "C" int uninet_session_open(uninet_session_t* session) {
    try {
        return (session && session->session && session->session->open()) ? 1 : 0;
    } catch (...) { return 0; }
}

// ── large payloads ────────────────────────────────────────────────────────

struct uninet_blob {
    std::unique_ptr<Blob> blob;
};

extern "C" uninet_blob_t* uninet_blob_new(uninet_session_t* session, const char* subject) {
    try {
        if (!session || !session->session || !subject || !*subject) {
            set_error("null session or subject");
            return nullptr;
        }
        if (!session->session->open()) {
            set_error("the session is closed");
            return nullptr;
        }
        auto handle = std::unique_ptr<uninet_blob>(new uninet_blob());
        handle->blob = std::unique_ptr<Blob>(new Blob(*session->session, subject));
        return handle.release();
    } catch (const std::exception& e) { set_error(e.what()); return nullptr; }
      catch (...) { set_error("could not create the transfer channel"); return nullptr; }
}

extern "C" void uninet_blob_free(uninet_blob_t* blob) {
    try { delete blob; } catch (...) {}
}

namespace {

// Shared by send and send_file: parse the optional metadata, then report the id.
bool parse_meta(const char* meta_json, Cbor& out) {
    if (!meta_json || !*meta_json) { out = Cbor::null(); return true; }
    bool ok = false;
    out = from_json(meta_json, &ok);
    if (!ok) set_error("meta_json is not valid JSON");
    return ok;
}

}  // namespace

extern "C" int uninet_blob_send(uninet_blob_t* blob, const char* name,
                                const uint8_t* data, size_t len, const char* meta_json,
                                const char* dst, char* id_buf, size_t id_buflen) {
    try {
        if (!blob || !blob->blob || !name || (!data && len)) {
            set_error("null blob, name or payload");
            return UNINET_ERR_ARG;
        }
        Cbor meta;
        if (!parse_meta(meta_json, meta)) return UNINET_ERR_PARSE;
        const std::string id = blob->blob->send(name, data, len, std::move(meta), safe(dst));
        // copy_out("") returns 0, which is indistinguishable from UNINET_OK, so
        // a failed send looked like a successful one with an empty id.
        if (id.empty()) {
            set_error("the transfer could not start: not on the network, or the "
                      "payload exceeds the configured maximum");
            return UNINET_ERR_STATE;
        }
        return copy_out(id, id_buf, id_buflen);
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_blob_send_file(uninet_blob_t* blob, const char* path,
                                     const char* meta_json, const char* dst,
                                     char* id_buf, size_t id_buflen) {
    try {
        if (!blob || !blob->blob || !path) { set_error("null blob or path"); return UNINET_ERR_ARG; }
        Cbor meta;
        if (!parse_meta(meta_json, meta)) return UNINET_ERR_PARSE;
        const std::string id = blob->blob->send_file(path, std::move(meta), safe(dst));
        if (id.empty()) {
            set_error("could not read the file, or the send failed");
            return UNINET_ERR_STATE;
        }
        return copy_out(id, id_buf, id_buflen);
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_blob_on_received(uninet_blob_t* blob, uninet_blob_cb cb, void* user) {
    try {
        if (!blob || !blob->blob || !cb) { set_error("null argument"); return UNINET_ERR_ARG; }
        blob->blob->on_received([cb, user](const BlobInfo& info, const Bytes& data) {
            try {
                const std::string meta = to_json(info.meta);
                cb(info.id.c_str(), info.name.c_str(), info.src.c_str(), meta.c_str(),
                   data.data(), data.size(), user);
            } catch (...) {}
        });
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_blob_on_progress(uninet_blob_t* blob, uninet_blob_progress_cb cb,
                                       void* user) {
    try {
        if (!blob || !blob->blob || !cb) { set_error("null argument"); return UNINET_ERR_ARG; }
        blob->blob->on_progress([cb, user](const BlobInfo& info, size_t done) {
            try { cb(info.id.c_str(), info.name.c_str(), done, info.size, user); }
            catch (...) {}
        });
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_blob_on_failed(uninet_blob_t* blob, uninet_blob_failed_cb cb,
                                     void* user) {
    try {
        if (!blob || !blob->blob || !cb) { set_error("null argument"); return UNINET_ERR_ARG; }
        blob->blob->on_failed([cb, user](const BlobInfo& info, const std::string& why) {
            try { cb(info.id.c_str(), info.name.c_str(), why.c_str(), user); }
            catch (...) {}
        });
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_blob_incoming_count(uninet_blob_t* blob) {
    try { return (blob && blob->blob) ? int(blob->blob->incoming_count()) : 0; }
    catch (...) { return 0; }
}

// ── peer snapshot ─────────────────────────────────────────────────────────

extern "C" uninet_peers_t* uninet_session_peers(uninet_session_t* session) {
    try {
        if (!session || !session->session) { set_error("null session"); return nullptr; }
        auto snap = std::unique_ptr<uninet_peers>(new uninet_peers());
        snap->items = session->session->peers();
        return snap.release();
    } catch (...) { set_error("internal error"); return nullptr; }
}

extern "C" int uninet_peers_count(uninet_peers_t* peers) {
    return peers ? int(peers->items.size()) : 0;
}

namespace {

// An out-of-range index yields "" rather than reading past the end: this
// boundary is crossed by hand-written marshalling code, and a loop that runs one
// past the count should not be a crash.
bool valid(uninet_peers_t* peers, int index) {
    return peers && index >= 0 && size_t(index) < peers->items.size();
}

const char* header_of(uninet_peers_t* peers, int index, const char* key) {
    if (!valid(peers, index) || !key) return "";
    std::lock_guard<std::mutex> lk(peers->fields_mu);
    // Keyed per (index, key) so two live pointers into the same snapshot never
    // refer to the same storage.
    const std::string slot = std::to_string(index) + "\x1f" + key;
    auto it = peers->fields.find(slot);
    if (it == peers->fields.end())
        it = peers->fields.emplace(slot, peers->items[size_t(index)].header(key)).first;
    return it->second.c_str();
}

}  // namespace

extern "C" const char* uninet_peers_uuid(uninet_peers_t* peers, int index) {
    return valid(peers, index) ? peers->items[size_t(index)].uuid.c_str() : "";
}
extern "C" const char* uninet_peers_name(uninet_peers_t* peers, int index) {
    return valid(peers, index) ? peers->items[size_t(index)].name.c_str() : "";
}
extern "C" const char* uninet_peers_address(uninet_peers_t* peers, int index) {
    return valid(peers, index) ? peers->items[size_t(index)].address.c_str() : "";
}
extern "C" const char* uninet_peers_role(uninet_peers_t* peers, int index) {
    return header_of(peers, index, "role");
}
extern "C" const char* uninet_peers_app(uninet_peers_t* peers, int index) {
    return header_of(peers, index, "app");
}
extern "C" const char* uninet_peers_header(uninet_peers_t* peers, int index, const char* key) {
    return header_of(peers, index, key);
}

extern "C" int uninet_peers_has_header(uninet_peers_t* peers, int index, const char* key) {
    if (!valid(peers, index) || !key) return 0;
    return peers->items[size_t(index)].headers.count(key) ? 1 : 0;
}

extern "C" void uninet_peers_free(uninet_peers_t* peers) {
    try { delete peers; } catch (...) {}
}

// ── data conversion ───────────────────────────────────────────────────────

extern "C" int uninet_json_to_cbor(const char* json, uint8_t* out, size_t outlen,
                                   size_t* written) {
    try {
        if (!json) { set_error("null json"); return UNINET_ERR_ARG; }
        bool ok = false;
        Cbor v = from_json(json, &ok);
        if (!ok) { set_error("malformed JSON"); return UNINET_ERR_PARSE; }
        const Bytes bytes = encode(v);
        if (written) *written = bytes.size();
        // Probing for the required size is a legitimate call, not an error.
        if (!out || outlen == 0) return UNINET_OK;
        if (bytes.size() > outlen) {
            set_error("output buffer needs " + std::to_string(bytes.size()) + " bytes");
            return UNINET_ERR_BUFFER;
        }
        std::memcpy(out, bytes.data(), bytes.size());
        return UNINET_OK;
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

extern "C" int uninet_cbor_to_json(const uint8_t* cbor, size_t len, char* out, size_t outlen) {
    try {
        if (!cbor && len) { set_error("null cbor"); return UNINET_ERR_ARG; }
        bool ok = false;
        Cbor v = decode(cbor, len, &ok);
        if (!ok) { set_error("malformed CBOR"); return UNINET_ERR_PARSE; }
        return copy_out(to_json(v), out, outlen);
    } catch (...) { set_error("internal error"); return UNINET_ERR_INTERNAL; }
}

// ── diagnostics ───────────────────────────────────────────────────────────

extern "C" const char* uninet_last_error(void) {
    return g_error.c_str();   // std::string is never null-terminated-less; "" when unset
}

extern "C" uint16_t uninet_protocol_version(void) { return CURRENT_PROTOCOL_VERSION; }

extern "C" int uninet_has_lz4(void) {
#ifdef UNINET_HAS_LZ4
    return 1;
#else
    return 0;
#endif
}

extern "C" const char* uninet_version(void) {
    // Static storage: the caller may hold this pointer indefinitely.
    static const std::string v = zyre_version_string();
    return v.c_str();
}
