// uninet-discover — "what is on my network?", answered without asking the user
// for anything:
//
//     uninet-discover              watch devices arrive and leave, live
//     uninet-discover --once       print who is here right now, then exit
//
// This is the tool to run when someone says "the headset can't see the server".
// It lists the devices it can see and, when it sees none, says what to check.
#include "uninet/zyre_transport.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

double elapsed() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

std::string pad(std::string s, size_t w) {
    if (s.size() >= w) return s;
    s.append(w - s.size(), ' ');
    return s;
}

// "tcp://192.168.1.31:35001" -> "192.168.1.31"
std::string host_of(const std::string& endpoint) {
    std::string a = endpoint;
    const size_t scheme = a.find("://");
    if (scheme != std::string::npos) a = a.substr(scheme + 3);
    const size_t colon = a.rfind(':');
    if (colon != std::string::npos) a = a.substr(0, colon);
    return a;
}

void print_table(const std::vector<uninet::Peer>& peers) {
    if (peers.empty()) {
        std::printf("\nNo devices found.\n\n");
        std::printf("Things to check, in order:\n");
        std::printf("  1. Is the other device switched on and running its UniNet app?\n");
        std::printf("  2. Are both devices on the SAME Wi-Fi network or switch?\n");
        std::printf("  3. Guest Wi-Fi and \"client isolation\" stop devices from seeing\n");
        std::printf("     each other. A normal network, or a cable, will work.\n");
        std::printf("  4. A firewall may be blocking UDP port 5670.\n");
        std::printf("  5. On a machine with several networks, the app may be looking at\n");
        std::printf("     the wrong one — set the network interface in its settings.\n");
        return;
    }
    std::printf("\n%s %s %s %s\n",
                pad("DEVICE", 26).c_str(), pad("ADDRESS", 16).c_str(),
                pad("ROLE", 12).c_str(), "APP");
    for (const auto& p : peers) {
        std::printf("%s %s %s %s\n",
                    pad(p.name.empty() ? p.uuid : p.name, 26).c_str(),
                    pad(host_of(p.address), 16).c_str(),
                    pad(p.role().empty() ? "-" : p.role(), 12).c_str(),
                    p.app().empty() ? "-" : p.app().c_str());
    }
    std::printf("\n%zu device%s.\n", peers.size(), peers.size() == 1 ? "" : "s");
}

void usage() {
    std::printf(
        "uninet-discover — show the UniNet devices on this network\n\n"
        "  uninet-discover                 watch devices arrive and leave (Ctrl+C to stop)\n"
        "  uninet-discover --once          print who is here now, then exit\n\n"
        "Options:\n"
        "  --once                 one snapshot instead of a live view\n"
        "  --timeout <seconds>    how long --once listens (default 3)\n"
        "  --realm <name>         only show devices in this session group (default \"uninet\")\n"
        "  --interface <name>     network interface to look on, e.g. eth0 or 192.168.1.10\n"
        "  --version              print the ZeroMQ/Zyre versions in use\n"
        "  -h, --help             this message\n");
}

}  // namespace

int main(int argc, char** argv) {
    uninet::ZyreConfig cfg;
    bool once = false;
    double timeout_s = 3.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "uninet-discover: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--once")            once = true;
        else if (a == "--timeout")    timeout_s = std::atof(next("--timeout"));
        else if (a == "--realm")      cfg.realm = next("--realm");
        else if (a == "--interface")  cfg.iface = next("--interface");
        else if (a == "--version")    { std::printf("%s\n", uninet::zyre_version_string().c_str()); return 0; }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else {
            std::fprintf(stderr, "uninet-discover: unknown option '%s'\n\n", a.c_str());
            usage();
            return 2;
        }
    }

    // ZRE has no passive listener — you join the network to hear it. So this
    // tool appears in everyone else's list too, honestly labelled.
    cfg.headers["role"] = "observer";
    cfg.headers["app"]  = "uninet-discover";
    uninet::ZyreTransport net("uninet-discover", cfg);

    if (!once) {
        net.on_peer_found([](const uninet::Peer& p) {
            std::printf("[%6.1fs] + %s\n", elapsed(), p.describe().c_str());
            std::fflush(stdout);
        });
        net.on_peer_lost([](const uninet::Peer& p) {
            std::printf("[%6.1fs] - %s left\n", elapsed(),
                        (p.name.empty() ? p.uuid : p.name).c_str());
            std::fflush(stdout);
        });
    }

    if (!net.connect()) {
        std::fprintf(stderr, "Could not start looking for devices: %s\n",
                     net.last_error().c_str());
        return 1;
    }

    if (once) {
        std::this_thread::sleep_for(std::chrono::milliseconds(int(timeout_s * 1000)));
        print_table(net.peers());
        const bool empty = net.peers().empty();
        net.disconnect();
        return empty ? 1 : 0;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::printf("Looking for UniNet devices on your network");
    if (cfg.realm != "uninet") std::printf(" (session group \"%s\")", cfg.realm.c_str());
    std::printf("…\nPress Ctrl+C to stop.\n\n");
    std::fflush(stdout);

    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    print_table(net.peers());
    net.disconnect();
    return 0;
}
