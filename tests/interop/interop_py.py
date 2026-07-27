#!/usr/bin/env python3
"""UniNet cross-language interop: the Python participant.

See interop_cpp.cpp for what this proves. Run:

    python3 interop_py.py <realm> [seconds] [expected-peers-csv]
"""
from __future__ import annotations

import sys
import time

import uninet

# The one payload all three languages must produce identically. Kept in sync
# with interop_cpp.cpp and InteropCs/Program.cs by hand. That is the point: if
# a language encodes any of these differently, the comparison below fails.
def payload(lang: str) -> dict:
    return {
        "from": lang,
        "text": "Röntgen 20°C",
        "exact": 0.5,
        "inexact": 3.25,
        "count": 42,
        "neg": -7,
        "flag": True,
        "nothing": None,
        "pts": [1.5, 2.5, 3.5],
        "nested": {"a": 1, "b": [True, False]},
    }


def check(got: dict) -> str:
    """Return "" when `got` matches the reference, else the reason it does not."""
    want = payload("x")
    for key, expected in want.items():
        if key == "from":
            continue
        if key not in got:
            return f"field '{key}' missing"
        actual = got[key]
        if isinstance(expected, float):
            if not isinstance(actual, float) or abs(actual - expected) > 1e-12:
                return f"field '{key}': got {actual!r}, expected {expected!r}"
        elif isinstance(expected, bool):
            # bool before int: bool is a subclass of int in Python.
            if not isinstance(actual, bool) or actual != expected:
                return f"field '{key}': got {actual!r}, expected {expected!r}"
        elif isinstance(expected, int):
            # An integer must arrive as an integer, not a float.
            if isinstance(actual, bool) or not isinstance(actual, int) or actual != expected:
                return f"field '{key}': got {actual!r} ({type(actual).__name__}), expected int {expected!r}"
        else:
            if actual != expected:
                return f"field '{key}': got {actual!r}, expected {expected!r}"
    return ""


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: interop_py.py <realm> [seconds]", file=sys.stderr)
        return 2
    realm = sys.argv[1]
    seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    expected = [x for x in (sys.argv[3] if len(sys.argv) > 3 else "cpp,csharp").split(",") if x]

    net = uninet.join("python", role="interop", app="python", realm=realm)
    if not net.connected():
        print("python: could not join the network", file=sys.stderr)
        return 1

    results: dict[str, str] = {}

    def on_hello(msg):
        data = msg.data
        sender = data.get("from")
        if not isinstance(sender, str):
            return
        results.setdefault(sender, check(data))

    net.subscribe("interop.hello", on_hello)

    # Republish while waiting: the others start at different moments, and a
    # message sent before they joined reaches nobody.
    # Once satisfied, keep publishing through a short settle period rather than
    # exiting immediately: leaving the moment WE have heard everyone tears down
    # the session while the others may still be waiting on OUR payload.
    deadline = time.monotonic() + seconds
    settle_until = None
    while time.monotonic() < deadline:
        net.publish("interop.hello", payload("python"))
        if len(results) >= len(expected) and settle_until is None:
            settle_until = time.monotonic() + 3.0
        if settle_until is not None and time.monotonic() >= settle_until:
            break
        time.sleep(0.3)

    failures = 0
    for sender, why in sorted(results.items()):
        if why:
            print(f"python: FAIL payload from {sender}: {why}")
            failures += 1
        else:
            print(f"python: PASS payload from {sender} matched")
    for lang in expected:
        if lang not in results:
            print(f"python: MISSING never heard from {lang}")
            failures += 1

    print(f"python: {'ALL OK' if failures == 0 else 'FAILED'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
