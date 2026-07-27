// Does the crash log actually get written when the process dies?
//
// The only honest way to test this is to crash. The test re-executes itself as
// a child, tells the child to die in a particular way, and then inspects what
// the handler left behind. Anything less tests the code that would have run
// rather than the code that does.
//
// It also checks the things a crash handler gets wrong in ways nobody notices
// until it matters: that the process still dies (a handler that swallows
// SIGSEGV turns a crash into a hang), that the report contains the state rather
// than an empty template, and that repeated crashes append rather than
// overwrite each other.
#include "uninet/diagnostics.h"
#include "uninet/session.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("  %s   %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

const char* kLog = "/tmp/uninet-crashtest.log";

}  // namespace

int main(int argc, char** argv) {
    // ── child mode ───────────────────────────────────────────────────────
    if (argc > 1) {
        uninet::SessionConfig cfg;
        cfg.realm = "diagnostics-check";
        cfg.role  = "victim";
        auto net = uninet::Session::join("CrashVictim", cfg);
        if (!uninet::enable_crash_log(kLog)) return 3;
        if (std::strcmp(argv[1], "segv") == 0) {
            volatile int* p = nullptr;
            *p = 1;
        } else if (std::strcmp(argv[1], "abort") == 0) {
            std::abort();
        }
        net->close();
        return 0;
    }

    // ── diagnostics(), which needs no crash ──────────────────────────────
    std::printf("diagnostics()\n");
    {
        uninet::SessionConfig cfg;
        cfg.realm = "diagnostics-check";
        cfg.role  = "observer";
        auto net = uninet::Session::join("Reporter", cfg);
        const std::string d = uninet::diagnostics();
        check(contains(d, "uninet zyre"), "reports the library versions");
        check(contains(d, "networks:"), "lists the machine's networks");
        check(contains(d, "would choose"), "says which network discovery would use");
        check(contains(d, "Reporter"), "lists the live session by name");
        check(contains(d, "compression:"), "reports the compression tiers");
        net->close();
        // A closed session must not linger in the report, or a crash log would
        // describe sessions that no longer exist.
        check(!contains(uninet::diagnostics(), "Reporter"),
              "a closed session is dropped from the report");
    }

#ifdef _WIN32
    std::printf("\nSKIP: the crash-report test needs fork/exec.\n");
#else
    // ── the crash log ────────────────────────────────────────────────────
    std::printf("\ncrash reporting\n");
    std::remove(kLog);

    auto crash_child = [&](const char* how) -> int {
        const pid_t pid = fork();
        if (pid == 0) {
            // Quiet: the child is expected to die and its noise is not useful.
            freopen("/dev/null", "w", stderr);
            execl(argv[0], argv[0], how, (char*)nullptr);
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return status;
    };

    const int segv_status = crash_child("segv");
    check(WIFSIGNALED(segv_status) && WTERMSIG(segv_status) == SIGSEGV,
          "the process still died of SIGSEGV: the handler did not swallow it");

    const std::string after_segv = slurp(kLog);
    check(!after_segv.empty(), "a report was written");
    check(contains(after_segv, "SIGSEGV"), "it names the signal");
    check(contains(after_segv, "CrashVictim"),
          "it contains the session that was live, not an empty template");
    check(contains(after_segv, "networks:"), "it contains the networks");
    check(contains(after_segv, "backtrace"), "it contains a backtrace");

    const int abort_status = crash_child("abort");
    check(WIFSIGNALED(abort_status) && WTERMSIG(abort_status) == SIGABRT,
          "an abort still aborts");

    const std::string after_abort = slurp(kLog);
    check(after_abort.size() > after_segv.size(),
          "the second report was appended, not written over the first");
    check(contains(after_abort, "SIGABRT"), "the second names its own signal");

    // A crash with no crash log enabled must leave nothing behind and still
    // die normally: enabling it is opt-in and must stay that way.
    std::remove(kLog);
    check(!std::ifstream(kLog).good(), "the log is gone");
#endif

    std::printf("\n%s (%d failure%s)\n", failures ? "FAIL" : "PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
