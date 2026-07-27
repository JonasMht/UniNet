// UniNet: diagnostics and crash reporting. See include/uninet/diagnostics.h.
//
// The whole design is shaped by one constraint: a signal handler may call
// almost nothing. No malloc, no std::string, no locks, no printf. Anything that
// allocates can deadlock if the crash happened inside the allocator, which is
// exactly where memory-corruption crashes tend to happen. So the report is
// rendered into a fixed buffer AHEAD of time, whenever the state changes, and
// the handler does nothing but write() bytes that already exist.
#include "uninet/diagnostics.h"

#include "uninet/session.h"
#include "uninet/zyre_transport.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(__APPLE__)
#include <execinfo.h>
#define UNINET_HAVE_BACKTRACE 1
#endif
#endif

namespace uninet {
namespace {

// ── the live session registry ────────────────────────────────────────────
std::mutex& registry_mu() {
    static std::mutex m;
    return m;
}
std::vector<const Session*>& registry() {
    static std::vector<const Session*> v;
    return v;
}

// ── the pre-rendered crash snapshot ──────────────────────────────────────
// Fixed size: a signal handler must not allocate, so this is all there is. Big
// enough for a machine with a dozen networks and a dozen sessions; anything
// beyond that is truncated rather than grown.
constexpr size_t kSnapshotMax = 16 * 1024;

// Double-buffered, and that is not over-engineering.
//
// Writers are several: every session refreshes this when it joins, closes or
// rebuilds, and on a machine that changes network every session's actor thread
// does so at once. ThreadSanitizer found three races here from one run.
//
// A mutex fixes the writers but cannot help the READER, because the reader is a
// signal handler and a handler must not take a lock: if the crash happened
// while a writer held it, the report would deadlock instead of being written.
// So writers serialise on the mutex and fill the INACTIVE buffer, then publish
// it by flipping an atomic index. The handler reads whichever buffer is current
// and is guaranteed a complete report rather than half of two.
char g_snapshot[2][kSnapshotMax];
std::atomic<size_t> g_snapshot_len[2];
std::atomic<int> g_active{0};
std::atomic<bool> g_crash_enabled{false};
char g_crash_path[4096];

std::mutex& snapshot_mu() {
    static std::mutex m;
    return m;
}

void render_snapshot(const std::string& text) {
    std::lock_guard<std::mutex> lk(snapshot_mu());
    const int next = 1 - g_active.load(std::memory_order_relaxed);
    const size_t n = text.size() < kSnapshotMax - 1 ? text.size() : kSnapshotMax - 1;
    std::memcpy(g_snapshot[next], text.data(), n);
    g_snapshot[next][n] = '\0';
    g_snapshot_len[next].store(n, std::memory_order_release);
    g_active.store(next, std::memory_order_release);
}

#ifndef _WIN32
// write() can return short. Losing the tail of a crash report to a partial
// write would be a poor way to find out about that.
void write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        const ssize_t n = ::write(fd, data, len);
        if (n <= 0) return;
        data += n;
        len -= size_t(n);
    }
}
void write_str(int fd, const char* s) { write_all(fd, s, std::strlen(s)); }

// No snprintf in a signal handler either: it can allocate for some formats.
void write_int(int fd, long long v) {
    char buf[32];
    int i = int(sizeof buf);
    const bool neg = v < 0;
    unsigned long long u = neg ? 0ull - (unsigned long long)v : (unsigned long long)v;
    if (u == 0) buf[--i] = '0';
    while (u > 0 && i > 0) { buf[--i] = char('0' + (u % 10)); u /= 10; }
    if (neg && i > 0) buf[--i] = '-';
    write_all(fd, buf + i, size_t(int(sizeof buf) - i));
}

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGABRT: return "SIGABRT (abort, often a failed assertion or uncaught exception)";
        case SIGBUS:  return "SIGBUS (misaligned or unmapped access)";
        case SIGFPE:  return "SIGFPE (arithmetic fault)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        default:      return "fatal signal";
    }
}

struct sigaction g_previous[NSIG];
bool g_installed[NSIG];

extern "C" void crash_handler(int sig, siginfo_t* info, void* ctx) {
    if (g_crash_enabled.load(std::memory_order_acquire)) {
        const int fd = ::open(g_crash_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write_str(fd, "\n==================== UniNet crash report ====================\n");
            write_str(fd, "signal : ");
            write_str(fd, signal_name(sig));
            write_str(fd, "\npid    : ");
            write_int(fd, (long long)::getpid());
            if (info) {
                write_str(fd, "\nat     : 0x");
                // Hex by hand: this is the faulting address, and it is the
                // single most useful number in the whole report.
                unsigned long long a = (unsigned long long)(uintptr_t)info->si_addr;
                char hex[17];
                int i = 16;
                hex[16] = '\0';
                if (a == 0) { write_str(fd, "0"); }
                else {
                    while (a > 0 && i > 0) {
                        const int d = int(a & 0xF);
                        hex[--i] = char(d < 10 ? '0' + d : 'a' + d - 10);
                        a >>= 4;
                    }
                    write_str(fd, hex + i);
                }
            }
            write_str(fd, "\n\n-- UniNet state at the last change --\n");
            const int active = g_active.load(std::memory_order_acquire);
            write_all(fd, g_snapshot[active],
                      g_snapshot_len[active].load(std::memory_order_acquire));

#ifdef UNINET_HAVE_BACKTRACE
            // backtrace_symbols_fd is the one backtrace call glibc documents as
            // usable here: unlike backtrace_symbols it does not malloc.
            write_str(fd, "\n-- backtrace --\n");
            void* frames[64];
            const int n = ::backtrace(frames, 64);
            ::backtrace_symbols_fd(frames, n, fd);
#else
            write_str(fd, "\n(no backtrace available in this build)\n");
#endif
            write_str(fd, "=============================================================\n");
            ::close(fd);
        }
    }
    // Chain to whatever was there before, so a host application's own crash
    // reporter still runs. If there was none, restore the default and re-raise
    // so the process dies the way it would have: swallowing a SIGSEGV would
    // turn a crash into a hang.
    if (sig >= 0 && sig < NSIG && g_installed[sig]) {
        struct sigaction& prev = g_previous[sig];
        if ((prev.sa_flags & SA_SIGINFO) && prev.sa_sigaction) {
            prev.sa_sigaction(sig, info, ctx);
            return;
        }
        if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
            prev.sa_handler(sig);
            return;
        }
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}
#endif  // !_WIN32

}  // namespace

// ── registry hooks, called by Session ────────────────────────────────────
void diagnostics_register(const Session* s) {
    {
        std::lock_guard<std::mutex> lk(registry_mu());
        registry().push_back(s);
    }
    render_snapshot(diagnostics());
}

void diagnostics_unregister(const Session* s) {
    {
        std::lock_guard<std::mutex> lk(registry_mu());
        auto& v = registry();
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i] == s) { v.erase(v.begin() + long(i)); break; }
    }
    render_snapshot(diagnostics());
}

void diagnostics_refresh() { render_snapshot(diagnostics()); }

std::string diagnostics() {
    std::string out;
    out += "uninet " + zyre_version_string() + "\n";
    out += std::string("compression: zlib ")
#ifdef UNINET_HAS_ZLIB
           + "yes"
#else
           + "NO"
#endif
           + ", lz4 "
#ifdef UNINET_HAS_LZ4
           + "yes"
#else
           + "NO (messages from a peer that has it will be dropped)"
#endif
           + "\n";
    out += "host: " + local_hostname() + "\n";

    out += "\nnetworks:\n";
    const auto ifaces = local_interfaces();
    if (ifaces.empty()) out += "  (none)\n";
    for (const auto& i : ifaces) {
        out += "  " + i.name + "  " + i.address + "  " + link_kind_name(i.kind);
        if (!i.is_discoverable()) out += "  [not used for discovery]";
        else if (i.is_private())  out += "  [private]";
        out += "\n";
    }
    const Interface best = best_interface(ifaces);
    out += "  -> would choose: " + (best.name.empty() ? std::string("(none)") : best.name) + "\n";

    out += "\nsessions:\n";
    std::vector<const Session*> live;
    {
        std::lock_guard<std::mutex> lk(registry_mu());
        live = registry();
    }
    if (live.empty()) out += "  (none)\n";
    for (const Session* s : live) {
        // const_cast because transport() is non-const; nothing here mutates.
        Session* m = const_cast<Session*>(s);
        out += "  " + s->name();
        if (!s->open()) { out += "  [closed]\n"; continue; }
        out += s->connected() ? "  connected" : "  NOT connected";
        try {
            const auto& t = m->transport();
            out += "  on " + (t.chosen_interface().name.empty()
                                  ? std::string("(unset/gossip)") : t.chosen_interface().name);
            out += "  uuid " + s->uuid();
            out += "  peers " + std::to_string(s->peers().size());
            out += "  reconnects " + std::to_string(t.reconnect_count());
            const std::string err = t.last_error();
            if (!err.empty()) out += "\n      last error: " + err;
        } catch (const std::exception&) {
            out += "  [closing]";
        }
        out += "\n";
    }
    return out;
}

#ifdef _WIN32
namespace {
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    if (g_crash_enabled.load(std::memory_order_acquire)) {
        HANDLE h = CreateFileA(g_crash_path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            const char* head = "\n============ UniNet crash report ============\n";
            WriteFile(h, head, DWORD(std::strlen(head)), &written, nullptr);
            char code[64];
            const int n = std::snprintf(code, sizeof code, "exception: 0x%08lx\n\n",
                                        info ? info->ExceptionRecord->ExceptionCode : 0);
            if (n > 0) WriteFile(h, code, DWORD(n), &written, nullptr);
            const int active = g_active.load(std::memory_order_acquire);
            WriteFile(h, g_snapshot[active], DWORD(g_snapshot_len[active].load()),
                      &written, nullptr);
            const char* tail = "=============================================\n";
            WriteFile(h, tail, DWORD(std::strlen(tail)), &written, nullptr);
            CloseHandle(h);
        }
    }
    return g_previous_filter ? g_previous_filter(info) : EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

bool enable_crash_log(const std::string& path) {
    if (path.empty() || path.size() >= sizeof g_crash_path) return false;
    std::memcpy(g_crash_path, path.c_str(), path.size() + 1);
    render_snapshot(diagnostics());
    g_previous_filter = SetUnhandledExceptionFilter(crash_filter);
    g_crash_enabled.store(true, std::memory_order_release);
    return true;
}

void disable_crash_log() {
    if (!g_crash_enabled.exchange(false)) return;
    SetUnhandledExceptionFilter(g_previous_filter);
    g_previous_filter = nullptr;
}
#else
bool enable_crash_log(const std::string& path) {
    if (path.empty() || path.size() >= sizeof g_crash_path) return false;
    std::memcpy(g_crash_path, path.c_str(), path.size() + 1);
    render_snapshot(diagnostics());

    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    // SA_ONSTACK matters for SIGSEGV: a stack overflow is reported as one, and
    // without an alternate stack the handler cannot run to report it.
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;

    // A fixed size, not SIGSTKSZ: on current glibc that expands to
    // sysconf(_SC_SIGSTKSZ) and is no longer a compile-time constant, so an
    // array sized by it is a variable-length array and cannot be static.
    static char alt_stack[64 * 1024];
    stack_t ss;
    std::memset(&ss, 0, sizeof ss);
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof alt_stack;
    sigaltstack(&ss, nullptr);

    bool any = false;
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
        if (sigaction(sig, &sa, &g_previous[sig]) == 0) {
            g_installed[sig] = true;
            any = true;
        }
    }
    if (any) g_crash_enabled.store(true, std::memory_order_release);
    return any;
}

void disable_crash_log() {
    if (!g_crash_enabled.exchange(false)) return;
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
        if (g_installed[sig]) {
            sigaction(sig, &g_previous[sig], nullptr);
            g_installed[sig] = false;
        }
    }
}
#endif

}  // namespace uninet
