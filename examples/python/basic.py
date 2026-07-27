#!/usr/bin/env python3
"""UniNet: connecting two nodes.

The smallest complete program. Run it in two terminals:

    python3 basic.py alice
    python3 basic.py bob

Nobody types an address. The two find each other and start talking.
"""
import sys
import time

import uninet


def main() -> int:
    name = sys.argv[1] if len(sys.argv) > 1 else "python-node"

    # ── the entire setup ──
    net = uninet.join(name, role="demo", app="uninet-examples")
    print(net.describe())

    # Who else is here: fires for devices already present, and as they arrive.
    net.on_peer_found(lambda p: print(f"  + {p.name} at {p.endpoint} on {p.host}"))
    net.on_peer_lost(lambda p: print(f"  - {p.name} left"))

    # What arrives. The payload is a plain dict.
    net.subscribe("chat.>", lambda msg: print(f"  {msg.data['from']}: {msg.data['text']}"))

    print("Talking on subject 'chat.room'. Ctrl+C to stop.\n")
    tick = 0
    try:
        while True:
            time.sleep(2)
            tick += 1
            net.publish("chat.room", {"from": name, "text": f"hello #{tick}"})
            print(f"[{len(net.peers())} peer(s)] sent hello #{tick}")
    except KeyboardInterrupt:
        print("\nleaving: the others see it immediately")
    return 0


if __name__ == "__main__":
    sys.exit(main())
