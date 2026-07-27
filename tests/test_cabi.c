/* UniNet: C ABI test.
 *
 * Compiled as C, not C++, so it proves the header is actually usable from a C
 * compiler and that the extern "C" surface links without the C++ runtime being
 * pulled in by the caller. This is the exact path the C#/Unity binding takes
 * through P/Invoke, so if this passes, the marshalling contract is sound.
 */
#include "uninet/cabi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  define SLEEP_MS(ms) Sleep(ms)
#  define GETPID() ((int)GetCurrentProcessId())
#else
#  include <unistd.h>
#  define SLEEP_MS(ms) usleep((ms) * 1000)
#  define GETPID() ((int)getpid())
#endif

static int g_failures = 0;

static void check(int cond, const char* what) {
    printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) ++g_failures;
}

/* Discovery is a network event; poll rather than assert immediately. */
static int wait_until(int (*pred)(void*), void* arg, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        if (pred(arg)) return 1;
        SLEEP_MS(50);
        waited += 50;
    }
    return pred(arg);
}

/* ── collected callback state ── */
typedef struct {
    int  blobs;
    char blob_name[128];
    char blob_meta[256];
    int  blob_bytes;
    int  blob_ok;              /* payload matched the expected pattern */
    int  blob_progress_calls;
    int  cbor_messages;
    int  messages;
    char last_subject[128];
    char last_json[512];
    char last_src[128];
    int  peers_found;
    char last_peer_name[128];
    char last_peer_role[64];
} Collected;

static void on_json(const char* subject, const char* src_uuid, const char* json, void* user) {
    Collected* c = (Collected*)user;
    snprintf(c->last_subject, sizeof c->last_subject, "%s", subject ? subject : "");
    snprintf(c->last_json, sizeof c->last_json, "%s", json ? json : "");
    snprintf(c->last_src, sizeof c->last_src, "%s", src_uuid ? src_uuid : "");
    ++c->messages;   /* written last: the strings are complete before the count moves */
}

static void on_peer(const char* uuid, const char* name, const char* address,
                    const char* role, const char* app, void* user) {
    Collected* c = (Collected*)user;
    (void)uuid; (void)address; (void)app;
    snprintf(c->last_peer_name, sizeof c->last_peer_name, "%s", name ? name : "");
    snprintf(c->last_peer_role, sizeof c->last_peer_role, "%s", role ? role : "");
    ++c->peers_found;
}

/* ── predicates for wait_until ── */
static int has_message(void* arg)  { return ((Collected*)arg)->messages > 0; }
static int has_blob(void* arg)     { return ((Collected*)arg)->blobs > 0; }
static int has_cbor(void* arg)     { return ((Collected*)arg)->cbor_messages > 0; }

static void on_cbor(const char* subject, const char* src, const uint8_t* data,
                    size_t len, void* user) {
    Collected* c = (Collected*)user;
    (void)subject; (void)src;
    /* CBOR bytes must be non-empty and decodable back to JSON. */
    char json[256];
    if (len > 0 && uninet_cbor_to_json(data, len, json, sizeof json) > 0) ++c->cbor_messages;
}

static void on_blob(const char* id, const char* name, const char* src,
                    const char* meta_json, const uint8_t* data, size_t len, void* user) {
    Collected* c = (Collected*)user;
    (void)id; (void)src;
    snprintf(c->blob_name, sizeof c->blob_name, "%s", name ? name : "");
    snprintf(c->blob_meta, sizeof c->blob_meta, "%s", meta_json ? meta_json : "");
    c->blob_bytes = (int)len;
    c->blob_ok = 1;
    for (size_t i = 0; i < len; ++i)
        if (data[i] != (uint8_t)((i * 31 + 7) & 0xFF)) { c->blob_ok = 0; break; }
    ++c->blobs;
}

static void on_blob_progress(const char* id, const char* name, size_t done,
                             size_t total, void* user) {
    Collected* c = (Collected*)user;
    (void)id; (void)name; (void)done; (void)total;
    ++c->blob_progress_calls;
}
static int has_peer(void* arg)     { return ((Collected*)arg)->peers_found > 0; }

typedef struct { uninet_session_t* s; int want; } PeerCount;
static int peer_count_is(void* arg) {
    PeerCount* pc = (PeerCount*)arg;
    uninet_peers_t* peers = uninet_session_peers(pc->s);
    int n = uninet_peers_count(peers);
    uninet_peers_free(peers);
    return n == pc->want;
}

int main(void) {
    char realm[128];
    snprintf(realm, sizeof realm, "uninet-ctest-%d", GETPID());

    printf("UniNet C ABI test: %s\n\n", uninet_version());

    /* ── conversion, no network ── */
    printf("json <-> cbor\n");
    {
        unsigned char cbor[256];
        size_t written = 0;
        char json[256];
        int rc = uninet_json_to_cbor("{\"code\":\"update\",\"n\":42}", cbor, sizeof cbor, &written);
        check(rc == UNINET_OK && written > 0, "JSON converts to CBOR");

        rc = uninet_cbor_to_json(cbor, written, json, sizeof json);
        check(rc > 0, "CBOR converts back to JSON");
        check(strcmp(json, "{\"code\":\"update\",\"n\":42}") == 0,
              "the round trip is byte-identical");

        rc = uninet_json_to_cbor("{not json}", cbor, sizeof cbor, &written);
        check(rc == UNINET_ERR_PARSE, "malformed JSON is refused with a specific code");
        check(strlen(uninet_last_error()) > 0, "and last_error explains why");

        /* A caller may probe for the required size with a NULL buffer. */
        written = 0;
        rc = uninet_json_to_cbor("{\"a\":1}", NULL, 0, &written);
        check(rc == UNINET_OK && written > 0, "size can be probed before allocating");

        char tiny[4];
        rc = uninet_cbor_to_json(cbor, 3, tiny, sizeof tiny);
        check(rc < 0, "an undersized buffer is an error, not a truncated string");
    }

    /* ── null-safety: the ABI must not crash on bad input ── */
    printf("null safety\n");
    {
        check(uninet_session_connected(NULL) == 0, "connected(NULL) is false, not a crash");
        check(uninet_session_uuid(NULL, NULL, 0) < 0, "uuid(NULL) is an error");
        check(uninet_session_publish_json(NULL, "s", "{}", NULL) < 0, "publish(NULL) is an error");
        check(uninet_peers_count(NULL) == 0, "peers_count(NULL) is 0");
        check(strcmp(uninet_peers_uuid(NULL, 0), "") == 0, "peer field of NULL is empty");
        uninet_session_free(NULL);       /* must not crash */
        uninet_peers_free(NULL);
        check(1, "free(NULL) is safe");
        check(uninet_session_join(NULL, NULL, NULL, NULL, NULL, 0) == NULL,
              "join without a name is refused");
    }

    /* ── two sessions, no address configured anywhere ── */
    printf("discovery and messaging\n");
    {
        Collected got;
        memset(&got, 0, sizeof got);

        uninet_session_t* a =
            uninet_session_join("C Sender", "server", "ctest", realm, NULL, 0);
        uninet_session_t* b =
            uninet_session_join("C Receiver", "viewer", "ctest", realm, NULL, 0);
        check(a != NULL && b != NULL, "both sessions were created");
        if (!a || !b) { printf("\nFAIL (cannot continue)\n"); return 1; }

        check(uninet_session_connected(a) == 1, "sender is on the network");
        check(uninet_session_connected(b) == 1, "receiver is on the network");

        char uuid[128];
        int n = uninet_session_uuid(a, uuid, sizeof uuid);
        check(n > 0 && strlen(uuid) == (size_t)n, "uuid is written with its length returned");

        char describe[256];
        check(uninet_session_describe(a, describe, sizeof describe) > 0,
              "describe() produces a status line");

        check(uninet_session_on_peer_found(a, on_peer, &got) == UNINET_OK,
              "presence callback registered");
        check(uninet_session_subscribe_json(b, "c.>", on_json, &got) == UNINET_OK,
              "subscription registered");

        PeerCount pc;
        pc.s = a; pc.want = 1;
        check(wait_until(peer_count_is, &pc, 25000), "the two sessions found each other");
        check(wait_until(has_peer, &got, 5000), "the peer-found callback fired");
        check(strcmp(got.last_peer_name, "C Receiver") == 0, "with the peer's name");
        check(strcmp(got.last_peer_role, "viewer") == 0, "and the role it advertised");

        check(uninet_session_publish_json(a, "c.hello", "{\"n\":1}", NULL) == UNINET_OK,
              "published a JSON message");
        check(wait_until(has_message, &got, 10000), "the message arrived");
        check(strcmp(got.last_subject, "c.hello") == 0, "on the right subject");
        check(strcmp(got.last_json, "{\"n\":1}") == 0, "with the payload intact");
        check(strcmp(got.last_src, uuid) == 0, "and the sender's uuid attached");

        /* Non-ASCII must survive: the boundary is UTF-8, not the code page. */
        got.messages = 0;
        check(uninet_session_publish_json(a, "c.utf8", "{\"name\":\"Röntgen: 20°\"}", NULL)
                  == UNINET_OK, "published a non-ASCII payload");
        check(wait_until(has_message, &got, 10000), "it arrived");
        check(strstr(got.last_json, "Röntgen") != NULL, "UTF-8 survived the boundary");

        /* Peer snapshot accessors. */
        uninet_peers_t* peers = uninet_session_peers(a);
        check(uninet_peers_count(peers) == 1, "snapshot holds one peer");
        check(strcmp(uninet_peers_name(peers, 0), "C Receiver") == 0, "name accessor");
        check(strlen(uninet_peers_address(peers, 0)) > 0, "address accessor");
        check(strcmp(uninet_peers_role(peers, 0), "viewer") == 0, "role accessor");
        check(strcmp(uninet_peers_app(peers, 0), "ctest") == 0, "app accessor");
        check(strcmp(uninet_peers_uuid(peers, 99), "") == 0,
              "an out-of-range index is empty, not a crash");

        /* The header documents these pointers as valid until the snapshot is
         * freed. Hold two at once and check the first still reads correctly -
         * a shared scratch buffer would have made `role` show the app name. */
        const char* role_ptr = uninet_peers_role(peers, 0);
        const char* app_ptr  = uninet_peers_app(peers, 0);
        check(strcmp(role_ptr, "viewer") == 0 && strcmp(app_ptr, "ctest") == 0,
              "two live field pointers do not alias each other");
        uninet_peers_free(peers);

        /* Malformed payloads are rejected rather than sent. */
        check(uninet_session_publish_json(a, "c.bad", "{oops}", NULL) == UNINET_ERR_PARSE,
              "malformed JSON is not published");

        uninet_session_free(b);
        uninet_session_free(a);
        check(1, "sessions closed cleanly");
    }

    /* ── config handle: headers and compression ── */
    printf("configuration\n");
    {
        uninet_config_t* cfg = uninet_config_new();
        check(cfg != NULL, "a config handle is created");
        check(uninet_config_set_header(cfg, "site", "lab-2") == UNINET_OK, "set_header");
        check(uninet_config_set_compression(cfg, 1) == UNINET_OK, "set_compression(zlib)");
        check(uninet_config_set_compression(cfg, 99) == UNINET_ERR_ARG,
              "an out-of-range compression is rejected, not cast blindly");
        check(uninet_config_set_port(cfg, 70000) == UNINET_ERR_ARG, "an out-of-range port is rejected");
        check(uninet_config_set_header(NULL, "k", "v") == UNINET_ERR_ARG, "null config is an error");
        uninet_config_free(cfg);
        uninet_config_free(NULL);
        check(1, "config free is safe");
    }

    /* ── the surface that shipped untested, where two bugs were hiding ── */
    printf("blob, cbor and lifetime\n");
    {
        Collected got;
        memset(&got, 0, sizeof got);

        uninet_config_t* cfg = uninet_config_new();
        uninet_config_set_realm(cfg, realm);
        uninet_config_set_role(cfg, "server");
        uninet_config_set_app(cfg, "ctest");
        uninet_config_set_header(cfg, "site", "lab-2");
        uninet_session_t* a = uninet_session_join_cfg("C Blob TX", cfg);
        uninet_session_t* b = uninet_session_join_cfg("C Blob RX", cfg);
        uninet_config_free(cfg);
        check(a != NULL && b != NULL, "sessions created from a config handle");
        if (!a || !b) { printf("\nFAIL (cannot continue)\n"); return 1; }

        check(uninet_session_open(a) == 1, "a new session reports open");

        /* An advertised header must be readable, and distinguishable from absent. */
        PeerCount pc; pc.s = a; pc.want = 1;
        check(wait_until(peer_count_is, &pc, 25000), "peers found each other");
        uninet_peers_t* peers = uninet_session_peers(a);
        check(strcmp(uninet_peers_header(peers, 0, "site"), "lab-2") == 0,
              "a custom header survives to the peer list");
        check(uninet_peers_has_header(peers, 0, "site") == 1, "has_header finds it");
        check(uninet_peers_has_header(peers, 0, "nope") == 0,
              "and reports an absent header as absent, not as empty");
        uninet_peers_free(peers);

        /* subscribe_cbor: was never covered by any test. */
        check(uninet_session_subscribe_cbor(b, "c.>", on_cbor, &got) == UNINET_OK,
              "subscribe_cbor registered");
        unsigned char cbor[64]; size_t written = 0;
        uninet_json_to_cbor("{\"n\":7}", cbor, sizeof cbor, &written);
        check(uninet_session_publish_cbor(a, "c.raw", cbor, written, NULL) == UNINET_OK,
              "publish_cbor accepted");
        check(wait_until(has_cbor, &got, 10000), "the CBOR message arrived and decoded");

        /* blob: the whole API was untested. */
        uninet_blob_t* tx = uninet_blob_new(a, "files");
        uninet_blob_t* rx = uninet_blob_new(b, "files");
        check(tx != NULL && rx != NULL, "blob channels created");
        check(uninet_blob_on_received(rx, on_blob, &got) == UNINET_OK, "on_received registered");
        check(uninet_blob_on_progress(rx, on_blob_progress, &got) == UNINET_OK,
              "on_progress registered");

        enum { kPayload = 700000 };
        uint8_t* payload = (uint8_t*)malloc(kPayload);
        check(payload != NULL, "payload allocated");
        for (int i = 0; i < kPayload; ++i) payload[i] = (uint8_t)((i * 31 + 7) & 0xFF);

        char id[128];
        int rc = uninet_blob_send(tx, "c.bin", payload, kPayload,
                                  "{\"kind\":\"test\",\"n\":42}", NULL, id, sizeof id);
        check(rc > 0, "blob send returned a transfer id");
        check(wait_until(has_blob, &got, 30000), "the blob arrived");
        check(strcmp(got.blob_name, "c.bin") == 0, "with its name");
        check(got.blob_bytes == kPayload, "and every byte");
        check(got.blob_ok == 1, "byte-for-byte identical");
        check(strstr(got.blob_meta, "\"n\":42") != NULL, "metadata survived as JSON");
        check(got.blob_progress_calls > 1, "progress was reported");
        check(uninet_blob_incoming_count(rx) == 0, "nothing left buffered");
        free(payload);

        /* Failures must be reported, not returned as UNINET_OK. */
        check(uninet_blob_send_file(tx, "/nonexistent/file", NULL, NULL, id, sizeof id) < 0,
              "sending a missing file is an error");

        /* close(): the operations that follow must be inert, not crashes, and
           registering on a closed session must not report success. */
        uninet_session_close(a);
        check(uninet_session_open(a) == 0, "close() marks the session closed");
        check(uninet_session_connected(a) == 0, "and disconnected");
        uninet_session_close(a);
        check(1, "close() is idempotent");
        check(uninet_session_publish_json(a, "t.x", "{}", NULL) < 0,
              "publish on a closed session is an error");
        check(uninet_session_subscribe_json(a, "t.>", on_json, &got) == UNINET_ERR_STATE,
              "subscribing on a closed session reports failure, not OK");
        check(uninet_session_on_peer_found(a, on_peer, &got) == UNINET_ERR_STATE,
              "registering presence on a closed session reports failure");

        uninet_blob_free(tx);
        uninet_blob_free(rx);
        uninet_blob_free(NULL);
        uninet_session_free(b);
        uninet_session_free(a);
        check(1, "everything freed cleanly");
    }

    printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
