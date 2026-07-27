#!/usr/bin/env python3
"""The workstation half of the UniNet Android demo, over USB.

    ./examples/android/usb-peer.sh          # sets up the cable, then runs this

This is examples/python/basic.py with one difference: a USB cable carries no
multicast, so there is no beacon for the two ends to find each other with.
Instead this side binds a rendezvous endpoint, the device dials it, and adb
forwards the three TCP connections that need to cross the cable. After that it
is ordinary UniNet: the same subjects, the same dicts, the same API.

Everything is loopback because that is what adb forwards. See usb-peer.sh.
"""
from __future__ import annotations

import argparse
import sys
import time

import uninet

# Both ends must agree on these. The device side is in MainActivity.java.
GOSSIP = "tcp://127.0.0.1:31337"   # rendezvous: bound here, dialled by the device
ENDPOINT = "tcp://127.0.0.1:31339"  # this machine's own mailbox


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--name", default="Workstation")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="exit after this long (0 = run until Ctrl+C)")
    ap.add_argument("--expect-peer", action="store_true",
                    help="exit non-zero unless a device connected AND a message "
                         "arrived from it: use this in a test")
    args = ap.parse_args()

    net = uninet.join(args.name, role="server", app="uninet-android-demo",
                      gossip_bind=GOSSIP, endpoint=ENDPOINT)
    print(net.describe())
    print(f"rendezvous: {GOSSIP}   waiting for the tablet")

    seen_peer: list[str] = []
    got_message: list[str] = []

    net.on_peer_found(lambda p: (seen_peer.append(p.name),
                                 print(f"  + {p.name} at {p.endpoint}"))[-1])
    net.on_peer_lost(lambda p: print(f"  - {p.name} left"))

    def on_chat(msg) -> None:
        got_message.append(msg.data.get("text", ""))
        print(f"  < {msg.data.get('from')}: {msg.data.get('text')}")

    net.subscribe("chat.>", on_chat)

    started = time.monotonic()
    tick = 0
    try:
        while args.seconds <= 0 or time.monotonic() - started < args.seconds:
            time.sleep(2)
            tick += 1
            net.publish("chat.room", {"from": args.name, "text": f"hello #{tick}"})
            print(f"  > hello #{tick}   [{len(net.peers())} peer(s)]")
    except KeyboardInterrupt:
        print("\nleaving")

    net.close()

    if args.expect_peer:
        # Both directions, deliberately. A device that connects and is heard
        # from still proves nothing about traffic going the other way, and that
        # is exactly the direction that was silently broken before.
        if not seen_peer:
            print("FAIL: no device ever connected", file=sys.stderr)
            return 1
        if not got_message:
            print("FAIL: the device connected but sent nothing", file=sys.stderr)
            return 1
        print(f"OK: {seen_peer[0]} connected and sent {len(got_message)} message(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
