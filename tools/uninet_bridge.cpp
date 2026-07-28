// uninet-bridge: joins ONE network and relays messages to and from its parent.
//
//     uninet-bridge --iface wlan0 --realm uninet --link ipc:///tmp/uninet-br-1
//
// WHY THIS IS A SEPARATE PROCESS, which is the whole design constraint.
//
// A machine on both Wi-Fi and a tethered phone can see devices on each, but
// they cannot see each other: discovery is link-local. Bridging means putting a
// node on every network and passing messages between them.
//
// One node per network cannot be done inside one process. Zyre picks its
// interface through czmq's zsys_interface, which is a single process-global
// string that Zyre's own threads read whenever they please. Two sessions in one
// process asking for different interfaces both get whichever was set last; that
// was measured, not assumed. No amount of locking fixes it, because the reading
// thread will never take our lock. So each extra network gets a process.
//
// The parent owns the primary network and talks to each bridge over a ZeroMQ
// PAIR socket. The topology is a star with the parent at the centre, which is
// what keeps the relay simple: a message arriving from one bridge is forwarded
// to the others and published locally, never back where it came from.
//
// Loops across MACHINES are the case a star cannot rule out: two laptops both
// bridging the same pair of networks would pass a message back and forth
// forever. Every relayed message therefore carries an id, and both ends drop
// one they have already handled.
#include "uninet/codec.h"
#include "uninet/session.h"

#include <czmq.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_set>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void usage() {
    std::printf(
        "uninet-bridge: relay messages between one network and a parent session\n\n"
        "  uninet-bridge (--iface <name> | --port <n>) --link <endpoint> [--realm <name>]\n\n"
        "Options:\n"
        "  --iface <name>     the network to join, e.g. wlan0\n"
        "  --port <n>         beacon port to join instead of, or as well as, an\n"
        "                     interface. A port isolates as completely as an\n"
        "                     interface does, which makes it the practical way to\n"
        "                     run two separate networks on one machine\n"
        "  --link <endpoint>  ZeroMQ PAIR endpoint the parent is bound to (required)\n"
        "  --realm <name>     session group (default \"uninet\")\n"
        "  --name <name>      what peers on that network see (default \"uninet-bridge\")\n"
        "  -h, --help         this message\n\n"
        "Started by UniNet itself when bridging is enabled. Running it by hand is\n"
        "useful mainly for debugging what a bridge can actually see.\n");
}

// Message ids recently relayed, so one that comes back around is recognised.
//
// Bounded and FIFO: unbounded would be a slow leak on a link that never goes
// quiet, and this only has to remember long enough for a message to travel a
// loop, which is milliseconds.
class SeenIds {
public:
    bool seen_before(const std::string& id) {
        if (id.empty()) return false;          // nothing to deduplicate on
        if (set_.count(id)) return true;
        set_.insert(id);
        order_.push_back(id);
        if (order_.size() > kMax) {
            set_.erase(order_.front());
            order_.pop_front();
        }
        return false;
    }

private:
    static constexpr size_t kMax = 8192;
    std::unordered_set<std::string> set_;
    std::deque<std::string> order_;
};

}  // namespace

int main(int argc, char** argv) {
    std::string iface, link, realm = "uninet", name = "uninet-bridge";
    int port = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--iface")       iface = next();
        else if (a == "--link")   link = next();
        else if (a == "--realm")  realm = next();
        else if (a == "--name")   name = next();
        else if (a == "--port")   port = std::atoi(next());
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option '%s'\n", a.c_str()); usage(); return 2; }
    }
    // One of the two is enough to say which network this bridge is for. A
    // beacon port isolates as completely as an interface does: nodes on 5670
    // never hear nodes on 5671, on the same wire.
    if ((iface.empty() && port == 0) || link.empty()) { usage(); return 2; }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    zsys_handler_set(nullptr);   // a helper does not own the process's signals

    // The link to the parent. PAIR because this is exactly two endpoints with
    // no routing to do, and it preserves message boundaries.
    zsock_t* link_sock = zsock_new_pair(link.c_str());
    if (!link_sock) {
        std::fprintf(stderr, "uninet-bridge: could not connect to %s\n", link.c_str());
        return 1;
    }

    uninet::SessionConfig cfg;
    cfg.realm = realm;
    if (!iface.empty()) cfg.iface = iface;   // the whole point: this process, this network
    if (port > 0) cfg.port = port;
    cfg.role  = "bridge";
    cfg.app   = "uninet-bridge";
    // A bridge that moved itself to another network would be relaying between
    // the wrong pair. The parent decides which network this process is for, and
    // restarts it if the machine's networks change.
    cfg.auto_reconnect = false;
    auto net = uninet::Session::join(name, cfg);
    if (!net->connected()) {
        std::fprintf(stderr, "uninet-bridge: could not join %s on %s: %s\n",
                     realm.c_str(), iface.c_str(), net->last_error().c_str());
        zsock_destroy(&link_sock);
        return 1;
    }

    SeenIds seen;

    // Network -> parent. Everything on this network, forwarded up as
    // [subject][mid][payload-as-CBOR-bytes].
    net->subscribe(">", [&](const uninet::Envelope& env) {
        if (seen.seen_before(env.mid)) return;
        // zsock_send from this thread is safe only because nothing else writes
        // to link_sock: the main loop below only reads.
        const uninet::Bytes body = uninet::encode(env.data);
        zmsg_t* m = zmsg_new();
        zmsg_addstr(m, env.subject.c_str());
        zmsg_addstr(m, env.mid.c_str());
        zmsg_addmem(m, body.data(), body.size());
        zmsg_send(&m, link_sock);
    });

    std::printf("uninet-bridge: %s on %s, relaying to %s\n", realm.c_str(),
                iface.empty() ? ("port " + std::to_string(port)).c_str() : iface.c_str(),
                link.c_str());
    std::fflush(stdout);

    // Parent -> network.
    zpoller_t* poller = zpoller_new(link_sock, nullptr);
    while (!g_stop.load() && !zsys_interrupted) {
        void* which = zpoller_wait(poller, 200);
        if (zpoller_terminated(poller)) break;
        if (which != link_sock) continue;

        zmsg_t* m = zmsg_recv(link_sock);
        if (!m) break;
        char* subject = zmsg_popstr(m);
        char* mid     = zmsg_popstr(m);
        zframe_t* body = zmsg_pop(m);
        if (subject && mid && body) {
            const std::string id = mid;
            if (!seen.seen_before(id)) {
                bool ok = false;
                uninet::Cbor data = uninet::decode(zframe_data(body), zframe_size(body), &ok);
                // The SAME id, deliberately. A fresh one would make this look
                // like a new message to every other bridge and the loop guard
                // would never fire.
                if (ok) net->publish(subject, std::move(data), "", id);
            }
        }
        freen(subject);
        freen(mid);
        zframe_destroy(&body);
        zmsg_destroy(&m);
    }

    zpoller_destroy(&poller);
    net->close();
    zsock_destroy(&link_sock);
    return 0;
}
