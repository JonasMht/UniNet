// UniNet demo: several devices finding each other and talking, with nothing
// configured by anyone.
//
// Run it in two terminals, or on two machines on the same network:
//
//     uninet-demo "Laptop"
//     uninet-demo "Headset" --role headset
//
// No address is typed. Neither instance is told the other exists. They find
// each other in about a second and start exchanging messages.
//
// scripts/demo.sh runs three of them at once.
//
// The interesting part of this file is how little of it is UniNet: three calls
// (join, subscribe, publish) and two optional presence callbacks. Everything
// else is printing.
#include "uninet/json.h"
#include "uninet/session.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

double elapsed() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void say(const std::string& line) {
    std::printf("[%5.1fs] %s\n", elapsed(), line.c_str());
    std::fflush(stdout);
}

void usage() {
    std::printf(
        "uninet-demo: devices finding each other with zero configuration\n\n"
        "  uninet-demo <device name> [options]\n\n"
        "Options:\n"
        "  --role <role>    what this device is: server / headset / viewer\n"
        "  --realm <name>   session group, to keep two setups apart (default \"uninet\")\n"
        "  --quiet          only print arrivals, departures and errors\n"
        "  -h, --help       this message\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    const std::string name = argv[1];
    if (name == "-h" || name == "--help") { usage(); return 0; }

    uninet::SessionConfig cfg;
    cfg.app = "uninet-demo";
    bool quiet = false;

    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--role")       cfg.role = next();
        else if (a == "--realm") cfg.realm = next();
        else if (a == "--quiet") quiet = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option '%s'\n", a.c_str()); usage(); return 2; }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ── the entire setup ──────────────────────────────────────────────────
    // No address, no port, no broker, no config file.
    auto net = uninet::Session::join(name, cfg);

    say(net->describe());
    if (!net->connected()) {
        std::fprintf(stderr,
            "\nCould not get onto the network. Check that this machine has an\n"
            "active network connection, then try again.\n");
        return 1;
    }

    // Who is here: now, and as devices come and go.
    net->on_peer_found([](const uninet::Peer& p) { say("JOINED  " + p.describe()); });
    net->on_peer_lost ([](const uninet::Peer& p) { say("LEFT    " + p.describe()); });

    // What arrives. The payload is CBOR on the wire; printing it as JSON is one
    // call, and a Python or C# peer sending the same data produces the same text.
    net->subscribe("demo.>", [](const uninet::Envelope& env) {
        say("MESSAGE on " + env.subject + ": " + uninet::to_json(env.data));
    });

    // What we send. JSON in, CBOR on the wire, JSON out at the other end: the
    // same bytes a C++ Cbor builder or a Python dict would have produced.
    int tick = 0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (g_stop.load()) break;
        ++tick;

        net->publish_json("demo.hello",
                          "{\"from\":\"" + name + "\",\"tick\":" + std::to_string(tick) + "}");

        if (!quiet) {
            const auto peers = net->peers();
            std::string line = std::to_string(peers.size()) +
                               (peers.size() == 1 ? " other device" : " other devices");
            for (const auto& p : peers) line += "\n             · " + p.describe();
            say(line);
        }
    }

    say("\"" + name + "\" leaving: the others will see it immediately.");
    return 0;
}
