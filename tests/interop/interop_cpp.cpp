// UniNet cross-language interop: the C++ participant.
//
// Three participants (C++, Python, C#) join one realm, each publishes an
// identical payload, and each verifies that what it receives from the other two
// decodes to exactly that payload. If the wire format diverged between
// languages, a float encoded differently, a UTF-8 string mangled, an integer
// silently promoted. This is where it shows.
//
//     interop_cpp <realm> [seconds] [expected-peers-csv]
//
// `expected-peers-csv` defaults to "python,csharp". A language that is not
// installed is passed out of the list rather than counted as a failure: a
// skipped participant is a gap in coverage, not a defect.
//
// Prints one PASS/FAIL line per peer seen, then a verdict. Exit 0 only if every
// expected peer was seen and every payload matched.
#include "uninet/json.h"
#include "uninet/session.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// The one payload all three languages must produce byte-identically. It is
// deliberately awkward: a non-ASCII string, an exact float, a float that is not
// exactly representable, an integer that must stay an integer, and a nested
// container.
const char* kPayloadJson =
    "{\"from\":\"%s\",\"text\":\"Röntgen 20°C\",\"exact\":0.5,\"inexact\":3.25,"
    "\"count\":42,\"neg\":-7,\"flag\":true,\"nothing\":null,"
    "\"pts\":[1.5,2.5,3.5],\"nested\":{\"a\":1,\"b\":[true,false]}}";

std::string payload_for(const std::string& lang) {
    char buf[512];
    std::snprintf(buf, sizeof buf, kPayloadJson, lang.c_str());
    return buf;
}

// Compare everything except "from", which necessarily differs per sender.
bool matches_expected(const uninet::Cbor& got, std::string& why) {
    bool ok = false;
    const uninet::Cbor want = uninet::from_json(payload_for("x"), &ok);
    if (!ok) { why = "reference payload did not parse"; return false; }

    for (const auto& kv : want.map_items()) {
        if (kv.first == "from") continue;
        const uninet::Cbor& g = got[kv.first];
        if (uninet::to_json(g) != uninet::to_json(kv.second)) {
            why = "field '" + kv.first + "' differs: got " + uninet::to_json(g) +
                  ", expected " + uninet::to_json(kv.second);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: interop_cpp <realm> [seconds] [expected-peers-csv]\n");
        return 2;
    }
    const std::string realm = argv[1];
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 20;

    std::vector<std::string> expected;
    {
        const std::string csv = argc > 3 ? argv[3] : "python,csharp";
        size_t start = 0;
        while (start <= csv.size()) {
            const size_t comma = csv.find(',', start);
            const std::string tok = csv.substr(start, comma - start);
            if (!tok.empty()) expected.push_back(tok);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    uninet::SessionConfig cfg;
    cfg.realm = realm;
    cfg.role  = "interop";
    cfg.app   = "cpp";
    auto net = uninet::Session::join("cpp", cfg);

    if (!net->connected()) {
        std::fprintf(stderr, "cpp: could not join the network\n");
        return 1;
    }

    std::mutex mu;
    std::map<std::string, std::string> results;   // sender -> "" (ok) or reason

    net->subscribe("interop.hello", [&](const uninet::Envelope& env) {
        const uninet::Cbor& from = env.data["from"];
        if (!from.is_text()) return;
        std::string why;
        const bool ok = matches_expected(env.data, why);
        std::lock_guard<std::mutex> lk(mu);
        if (!results.count(from.as_text())) results[from.as_text()] = ok ? "" : why;
    });

    // Republish periodically: participants start at different times, and a
    // message sent before the other side has joined reaches nobody.
    //
    // Once satisfied, keep publishing through a short settle period rather than
    // exiting immediately. Leaving the moment WE have heard everyone tears down
    // the session while the others may still be waiting on OUR payload: which
    // makes the run fail for whoever happened to finish last.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    auto settle_until = std::chrono::steady_clock::time_point::max();
    while (std::chrono::steady_clock::now() < deadline) {
        net->publish_json("interop.hello", payload_for("cpp"));
        {
            std::lock_guard<std::mutex> lk(mu);
            if (results.size() >= expected.size() &&
                settle_until == std::chrono::steady_clock::time_point::max())
                settle_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        }
        if (std::chrono::steady_clock::now() >= settle_until) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    std::lock_guard<std::mutex> lk(mu);
    int failures = 0;
    for (const auto& kv : results) {
        if (kv.second.empty()) {
            std::printf("cpp: PASS payload from %s matched\n", kv.first.c_str());
        } else {
            std::printf("cpp: FAIL payload from %s: %s\n", kv.first.c_str(), kv.second.c_str());
            ++failures;
        }
    }
    for (const auto& lang : expected) {
        if (!results.count(lang)) {
            std::printf("cpp: MISSING never heard from %s\n", lang.c_str());
            ++failures;
        }
    }
    std::printf("cpp: %s\n", failures == 0 ? "ALL OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
