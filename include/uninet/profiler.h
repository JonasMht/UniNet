// UniNet: opt-in performance analytics. Pinpoints where time goes in the
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
// Lock-free (a relaxed atomic load): every ScopedOp calls it, so taking a lock
// here serialized every thread in the pipeline even with the profiler off.
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
          armed_(enabled()), start_(armed_ ? clock::now() : time_point{}) {}
    ~ScopedOp() {
        // Only scopes that were timed may be recorded. Asking enabled() again here
        // meant a profiler switched on mid-scope reported clock::now() minus a
        // default-constructed time_point: machine uptime (245489 s was observed
        // in one report), which then dominated the table it was supposed to rank.
        if (armed_) {
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
    bool armed_;          // profiler was on when this scope opened
    time_point start_;
};

}  // namespace uninet::profiler
