// UniNet: sending a file to another machine, in C++.
//
//     uninet-file-transfer receive [output-dir]     # terminal 1
//     uninet-file-transfer send <path> [more...]    # terminal 2
//
// Or on two machines, one running each. No address is configured: the sender
// finds the receiver and streams the file, with progress at both ends.
//
// publish() would be the wrong tool here, a file has to be chunked, and the
// receiver wants to know it is 40% through. Blob does both.
#include "uninet/blob.h"
#include "uninet/session.h"

#include <atomic>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kSubject = "files";

std::string human(double n) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    int u = 0;
    while (n >= 1024.0 && u < 3) { n /= 1024.0; ++u; }
    char buf[48];
    std::snprintf(buf, sizeof buf, u == 0 ? "%.0f %s" : "%.1f %s", n, units[u]);
    return buf;
}

std::string basename_of(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

int receive(const std::string& outdir) {
    // Before joining, not when the first file lands: the directory defaults to
    // ./received, nothing creates it, and ofstream on a path in a directory
    // that does not exist simply fails - so a transfer that both ends reported
    // as complete left no file anywhere.
    std::error_code ec;
    std::filesystem::create_directories(outdir, ec);
    if (ec) {
        std::printf("Cannot create %s: %s\n", outdir.c_str(), ec.message().c_str());
        return 1;
    }

    auto net = uninet::Session::join("File Receiver", [] {
        uninet::SessionConfig c;
        c.role = "viewer";
        c.app  = "uninet-examples";
        return c;
    }());

    uninet::Blob blob(*net, kSubject);

    blob.on_progress([](const uninet::BlobInfo& info, size_t done) {
        const double pct = info.size ? 100.0 * double(done) / double(info.size) : 100.0;
        std::printf("\r  %s: %5.1f%%  %s/%s", info.name.c_str(), pct,
                    human(double(done)).c_str(), human(double(info.size)).c_str());
        std::fflush(stdout);
    });

    blob.on_received([outdir](const uninet::BlobInfo& info, const uninet::Bytes& data) {
        // Never trust a remote-supplied filename: taking the basename stops
        // "../../etc/passwd" from escaping the output directory.
        const std::string safe = basename_of(info.name).empty() ? "unnamed"
                                                                : basename_of(info.name);
        const std::string path = outdir + "/" + safe;
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            std::printf("\r  %s: could not write %s\n", info.name.c_str(), path.c_str());
            return;
        }
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        std::printf("\r  %s: saved to %s (%s)          \n",
                    info.name.c_str(), path.c_str(), human(double(data.size())).c_str());
    });

    blob.on_failed([](const uninet::BlobInfo& info, const std::string& why) {
        std::printf("\r  %s failed: %s\n", info.name.c_str(), why.c_str());
    });

    std::printf("%s\n", net->describe().c_str());
    std::printf("Saving incoming files to %s. Ctrl+C to stop.\n\n", outdir.c_str());

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}

int send(const std::vector<std::string>& paths) {
    auto net = uninet::Session::join("File Sender", [] {
        uninet::SessionConfig c;
        c.role = "server";
        c.app  = "uninet-examples";
        return c;
    }());

    uninet::Blob blob(*net, kSubject);

    std::printf("%s\nLooking for a receiver...\n", net->describe().c_str());
    for (int i = 0; i < 200 && net->peers().empty(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (net->peers().empty()) {
        std::printf("No receiver found. Start 'uninet-file-transfer receive' first.\n");
        return 1;
    }
    std::printf("Found %s at %s\n\n", net->peers()[0].name.c_str(),
                net->peers()[0].endpoint().c_str());

    for (const auto& path : paths) {
        std::ifstream probe(path, std::ios::binary | std::ios::ate);
        if (!probe) { std::printf("  skipping %s: cannot read it\n", path.c_str()); continue; }
        std::printf("  sending %s (%s)\n", basename_of(path).c_str(),
                    human(double(probe.tellg())).c_str());
        if (blob.send_file(path).empty())
            std::printf("    failed to start the transfer\n");
    }

    // Let the chunks drain before the session, and its network thread: goes away.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::printf("done\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "receive";
    if (mode == "receive") {
        return receive(argc > 2 ? argv[2] : "./received");
    }
    if (mode == "send" && argc > 2) {
        return send(std::vector<std::string>(argv + 2, argv + argc));
    }
    std::printf("uninet-file-transfer: send a file to another machine, no address needed\n\n"
                "  uninet-file-transfer receive [output-dir]\n"
                "  uninet-file-transfer send <path> [more...]\n");
    return 2;
}
