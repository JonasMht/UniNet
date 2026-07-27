// UniNet: what is this library doing right now, and what was it doing when the
// process died?
//
//     uninet::enable_crash_log("/var/log/myapp-uninet-crash.log");
//     ...
//     std::printf("%s", uninet::diagnostics().c_str());
//
// Two problems this solves, both of which have cost real time on this project:
//
//  1. "It cannot see the other device." The answer is almost always which
//     network was chosen, which realm was joined, or which compression tier the
//     two ends disagree on. All of that is knowable and none of it was
//     printable.
//
//  2. "It crashed on the headset and we have no idea why." A player on a Quest
//     or a module inside Slicer has no terminal, and the interesting state -
//     the networks, the peers, how many times it had reconnected - is gone by
//     the time anyone looks.
#pragma once

#include <string>

namespace uninet {

// A human-readable snapshot: version, compression tiers, every network on this
// machine and which one was chosen, and every live session with its realm,
// identity, peer count and reconnect count.
//
// Safe to call at any time from any thread. Cheap enough for a support button
// in a user interface, and it is exactly what to paste into a bug report.
std::string diagnostics();

// Write a crash report to `path` if the process dies on a fatal signal
// (SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL), or on an unhandled structured
// exception on Windows. Returns false if the handlers could not be installed.
//
// OFF BY DEFAULT, and deliberately opt-in. UniNet is a library: taking over an
// application's fatal signal handling without being asked is not its business,
// and a host like Unity or Slicer has its own crash reporting that must keep
// working. Any handler already installed is chained to afterwards, so enabling
// this does not disable theirs.
//
// The report contains the same information as diagnostics(), plus the signal
// and a backtrace. It is rendered in advance and refreshed whenever a session
// joins, reconnects or closes, because almost nothing is safe to do inside a
// signal handler: the handler itself only writes bytes that were already
// prepared. That means the snapshot can be a moment stale, which is a fair
// trade for a report that does not deadlock or crash while reporting a crash.
//
// Appends rather than truncates, so a device that crashes repeatedly leaves a
// history rather than only its last words.
bool enable_crash_log(const std::string& path);

// Restore whatever handlers were there before.
void disable_crash_log();

}  // namespace uninet
