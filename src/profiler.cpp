// UniNet: opt-in profiler implementation (mirrors UniVox's profiler).
#include "uninet/profiler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <vector>

namespace uninet::profiler {

namespace {
std::mutex g_mu;                        // guards g_stats only
// The flag is read twice per ScopedOp: i.e. on every encode, compress and
// dispatch, so it must not be behind g_mu: with the profiler OFF, that lock made
// every thread in the pipeline queue on a global mutex, which is the opposite of
// the "zero overhead when disabled" this header promises. Relaxed is enough: a
// racing enable() only decides whether a given scope is sampled.
std::atomic<bool> g_enabled{false};
std::map<std::string, OpStats> g_stats;
constexpr double kMB = 1024.0 * 1024.0;
}  // namespace

void enable(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool enabled()       { return g_enabled.load(std::memory_order_relaxed); }

void record(const char* op, double seconds, size_t bytes_in, size_t bytes_out) {
    if (!op) return;
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    OpStats& s = g_stats[op];
    s.count += 1;
    s.total_seconds += seconds;
    s.bytes_in += bytes_in;
    s.bytes_out += bytes_out;
}

Snapshot snapshot() {
    std::lock_guard<std::mutex> lk(g_mu);
    Snapshot sn;
    sn.ops = g_stats;
    return sn;
}

void reset() { std::lock_guard<std::mutex> lk(g_mu); g_stats.clear(); }

std::string report() {
    Snapshot sn = snapshot();
    if (sn.ops.empty()) return "(profiler: no samples recorded)\n";
    std::vector<std::pair<std::string, OpStats>> rows(sn.ops.begin(), sn.ops.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.total_seconds > b.second.total_seconds; });
    std::ostringstream o;
    o << "UniNet profiler (sorted by total time, dominant cost first):\n";
    o << "  op                       count    total(s)   mean(ms)   in(MB/s)   out(MB/s)\n";
    double grand = 0.0;
    for (const auto& [op, s] : rows) {
        grand += s.total_seconds;
        double mean_ms = s.count ? (s.total_seconds * 1000.0 / s.count) : 0.0;
        double in_mbs  = s.total_seconds > 0 ? (s.bytes_in  / kMB / s.total_seconds) : 0.0;
        double out_mbs = s.total_seconds > 0 ? (s.bytes_out / kMB / s.total_seconds) : 0.0;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "  %-22s %7zu   %9.4f   %8.3f   %8.1f   %8.1f\n",
                      op.c_str(), s.count, s.total_seconds, mean_ms, in_mbs, out_mbs);
        o << buf;
    }
    char tail[128];
    std::snprintf(tail, sizeof(tail), "  total wall in profiled ops: %.4f s\n", grand);
    o << tail;
    return o.str();
}

}  // namespace uninet::profiler
