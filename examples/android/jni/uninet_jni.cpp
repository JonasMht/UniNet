// JNI shim for the UniNet Android demo.
//
// This is the Android equivalent of examples/python/basic.py: join, see who is
// there, send and receive on "chat.room". The Java side owns the UI and does
// nothing else; everything here goes through UniNet's C ABI, exactly as the C#
// binding does.
//
// THREADING. UniNet's callbacks arrive on its network thread, which is a plain
// pthread with no JVM attached. Calling into JNI from there would need
// AttachCurrentThread and careful detach-on-exit, and getting it wrong crashes
// the process rather than throwing. So nothing here touches the JVM from a
// callback: the callbacks append to a mutex-protected queue of lines, and the
// Java side drains it from the UI thread on a timer. Same shape as Session.
// Update() in Unity, and the same reason.
#include <jni.h>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "uninet/cabi.h"

namespace {

std::mutex g_mu;
std::deque<std::string> g_lines;          // pending log lines, oldest first
uninet_session_t* g_session = nullptr;

// Bounded: a device left running for hours with nothing draining the queue
// should not grow it without limit.
constexpr size_t kMaxLines = 500;

void push(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_lines.size() >= kMaxLines) g_lines.pop_front();
    g_lines.push_back(line);
}

const char* safe(const char* s) { return s ? s : ""; }

void on_message(const char* subject, const char* src, const char* json, void*) {
    push(std::string("< ") + safe(subject) + "  " + safe(json));
}

void on_found(const char*, const char* name, const char* address,
              const char* role, const char*, void*) {
    push(std::string("+ ") + safe(name) + " (" + safe(role) + ") at " + safe(address));
}

void on_lost(const char*, const char* name, const char*, const char*, const char*, void*) {
    push(std::string("- ") + safe(name) + " left");
}

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out = p ? p : "";
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}

}  // namespace

extern "C" {

// Returns "" on success, or the reason it failed. Never throws across JNI.
JNIEXPORT jstring JNICALL
Java_org_uninet_demo_UniNet_nativeJoin(JNIEnv* env, jclass, jstring jname,
                                       jstring jgossip, jstring jendpoint) {
    if (g_session) return env->NewStringUTF("already joined");

    const std::string name = jstr(env, jname);
    const std::string gossip = jstr(env, jgossip);
    const std::string endpoint = jstr(env, jendpoint);

    // Empty gossip means ordinary Wi-Fi discovery (the UDP beacon). A non-empty
    // one is the USB path, where there is no multicast to beacon over.
    g_session = uninet_session_join_ex(
        name.c_str(), "demo", "uninet-android-demo", "uninet", nullptr, 0,
        nullptr,
        gossip.empty() ? nullptr : gossip.c_str(),
        endpoint.empty() ? nullptr : endpoint.c_str(),
        nullptr);
    if (!g_session) return env->NewStringUTF(safe(uninet_last_error()));

    if (uninet_session_on_peer_found(g_session, on_found, nullptr) != UNINET_OK ||
        uninet_session_on_peer_lost(g_session, on_lost, nullptr) != UNINET_OK ||
        uninet_session_subscribe_json(g_session, "chat.>", on_message, nullptr) != UNINET_OK) {
        const std::string why = safe(uninet_last_error());
        uninet_session_free(g_session);
        g_session = nullptr;
        return env->NewStringUTF(why.c_str());
    }
    return env->NewStringUTF("");
}

JNIEXPORT jstring JNICALL
Java_org_uninet_demo_UniNet_nativeDescribe(JNIEnv* env, jclass) {
    if (!g_session) return env->NewStringUTF("Not connected.");
    char buf[512];
    const int n = uninet_session_describe(g_session, buf, sizeof buf);
    return env->NewStringUTF(n >= 0 ? std::string(buf, size_t(n)).c_str() : "Not connected.");
}

JNIEXPORT jint JNICALL
Java_org_uninet_demo_UniNet_nativePeerCount(JNIEnv*, jclass) {
    if (!g_session) return 0;
    uninet_peers_t* peers = uninet_session_peers(g_session);
    if (!peers) return 0;
    const int n = uninet_peers_count(peers);
    uninet_peers_free(peers);
    return n;
}

JNIEXPORT jboolean JNICALL
Java_org_uninet_demo_UniNet_nativePublish(JNIEnv* env, jclass, jstring jtext) {
    if (!g_session) return JNI_FALSE;
    const std::string text = jstr(env, jtext);
    // Built by hand rather than with a JSON library: the payload is two known
    // fields, and the demo should not pull in a dependency to say so. Quotes
    // and backslashes are escaped so a typed message cannot produce invalid
    // JSON, which would be rejected at the far end with no clue why.
    std::string escaped;
    for (char c : text) {
        if (c == '"' || c == '\\') escaped += '\\';
        if (static_cast<unsigned char>(c) < 0x20) { escaped += ' '; continue; }
        escaped += c;
    }
    const std::string json = "{\"from\":\"android\",\"text\":\"" + escaped + "\"}";
    const bool ok = uninet_session_publish_json(g_session, "chat.room",
                                                json.c_str(), nullptr) == UNINET_OK;
    push(ok ? "> chat.room  " + json : std::string("! send failed: ") + safe(uninet_last_error()));
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Drains the queue. Returns the pending lines, oldest first; empty when idle.
JNIEXPORT jobjectArray JNICALL
Java_org_uninet_demo_UniNet_nativeDrain(JNIEnv* env, jclass) {
    std::vector<std::string> taken;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        taken.assign(g_lines.begin(), g_lines.end());
        g_lines.clear();
    }
    jobjectArray out = env->NewObjectArray(
        jsize(taken.size()), env->FindClass("java/lang/String"), nullptr);
    for (jsize i = 0; i < jsize(taken.size()); ++i) {
        jstring s = env->NewStringUTF(taken[size_t(i)].c_str());
        env->SetObjectArrayElement(out, i, s);
        env->DeleteLocalRef(s);   // the array holds its own reference
    }
    return out;
}

JNIEXPORT void JNICALL
Java_org_uninet_demo_UniNet_nativeLeave(JNIEnv*, jclass) {
    // Freed, not just closed: this is what tells peers immediately rather than
    // leaving them to time the device out 30 seconds later.
    uninet_session_t* s = g_session;
    g_session = nullptr;
    uninet_session_free(s);
}

JNIEXPORT jstring JNICALL
Java_org_uninet_demo_UniNet_nativeVersion(JNIEnv* env, jclass) {
    return env->NewStringUTF(safe(uninet_version()));
}

JNIEXPORT jboolean JNICALL
Java_org_uninet_demo_UniNet_nativeHasLz4(JNIEnv*, jclass) {
    // Worth showing in the UI: a build without it silently drops every message
    // from a peer that has it, which is invisible from the device otherwise.
    return uninet_has_lz4() == 1 ? JNI_TRUE : JNI_FALSE;
}

}  // extern "C"
