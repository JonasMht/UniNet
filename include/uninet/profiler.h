// UniNet — opt-in performance analytics. Pinpoints where time goes in the
// encode / compress / dispatch pipeline so the codec and transports can be tuned.
//
// Zero overhead when disabled (the default): ScopedOp reads no clock and records
// nothing. Enable with `profiler::enable(true)` (or `uninet.profiler.enable()`
// from Python), run a workload, then `profiler::report()` for a per-operation
// table (count, total/mean time, in/out throughput).
//
// ScopedOp is placed at the OPERATION level (per encode / compress / publish),
// not per byte, so the mutex in record() is never hot.
#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <string>

namespace uninet::profiler {

struct OpStats {
    size_t count = 0;
    double total_seconds = 0.0;   // sum of wall time
    size_t bytes_in = 0;          // bytes uncompressed (context-dependent)
    size_t bytes_out = 0;         // bytes compressed / on-wire
};

void enable(bool on = true);
bool enabled();
void record(const char* op, double seconds, size_t bytes_in, size_t bytes_out);

struct Snapshot {
    std::map<std::string, OpStats> ops;
};
Snapshot snapshot();
void reset();

// Human-readable bottleneck table. Sorted by total time (descending).
std::string report();

// RAII scoped timer. Use in hot paths:
//   { profiler::ScopedOp _("encode", src_bytes, dst_bytes); ...do work... }
class ScopedOp {
public:
    ScopedOp(const char* op, size_t bytes_in = 0, size_t bytes_out = 0)
        : op_(op), bytes_in_(bytes_in), bytes_out_(bytes_out),
          start_(enabled() ? clock::now() : time_point{}) {}
    ~ScopedOp() {
        if (enabled()) {
            double s = std::chrono::duration<double>(clock::now() - start_).count();
            record(op_, s, bytes_in_, bytes_out_);
        }
    }
    ScopedOp(const ScopedOp&) = delete;
    ScopedOp& operator=(const ScopedOp&) = delete;
    void set_bytes_in(size_t n)  { bytes_in_ = n; }
    void set_bytes_out(size_t n) { bytes_out_ = n; }

private:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;
    const char* op_;
    size_t bytes_in_, bytes_out_;
    time_point start_;
};

}  // namespace uninet::profiler
