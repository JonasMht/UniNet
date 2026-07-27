#!/usr/bin/env python3
"""UniNet: sending a 3D volume (numpy) to another machine.

A 3D volume is far too large for a single message, so it goes over `Blob`,
which chunks it, streams it, reassembles it, and reports progress at both ends.

The array's shape and dtype travel with it as metadata, so the receiver
reconstructs the exact array, no side channel, no agreed-in-advance layout.

    python3 volume_transfer.py receive        # terminal 1
    python3 volume_transfer.py send           # terminal 2

Or on two machines, one running each. Nobody types an address.
"""
import sys
import time

import numpy as np

import uninet

SUBJECT = "volumes"


def receive() -> int:
    net = uninet.join("Volume Receiver", role="viewer", app="uninet-examples")
    blob = uninet.Blob(net, SUBJECT)

    def on_progress(info, done):
        pct = 100.0 * done / info.size if info.size else 100.0
        print(f"\r  receiving {info.name}: {pct:5.1f}%  ({done:,}/{info.size:,} bytes)",
              end="", flush=True)

    def on_received(info, data):
        # Metadata carries everything needed to rebuild the array exactly.
        meta = info.meta
        volume = np.frombuffer(data, dtype=np.dtype(meta["dtype"]))
        volume = volume.reshape(meta["shape"])

        print(f"\r  received {info.name}: {volume.shape} {volume.dtype}"
              f"  ({info.size:,} bytes)          ")
        print(f"    spacing : {meta.get('spacing')}")
        print(f"    range   : [{volume.min():.3f}, {volume.max():.3f}]")
        print(f"    checksum: {int(volume.sum())}")

    def on_failed(info, why):
        print(f"\r  transfer of {info.name} failed: {why}")

    blob.on_progress(on_progress)
    blob.on_received(on_received)
    blob.on_failed(on_failed)

    print(net.describe())
    print(f"Waiting for volumes on '{SUBJECT}'. Ctrl+C to stop.\n")
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


def send() -> int:
    net = uninet.join("Volume Sender", role="server", app="uninet-examples")
    blob = uninet.Blob(net, SUBJECT)

    print(net.describe())
    print("Looking for a receiver...")
    for _ in range(200):
        if net.peers():
            break
        time.sleep(0.1)
    if not net.peers():
        print("No receiver found. Start 'volume_transfer.py receive' somewhere "
              "on this network first.")
        return 1
    print(f"Found {net.peers()[0].name} at {net.peers()[0].endpoint}\n")

    # A realistic volume: 256^3 float32 is 64 MiB.
    shape = (256, 256, 256)
    print(f"Building a {shape} float32 volume ({np.prod(shape) * 4 / 1e6:.0f} MB)...")
    rng = np.random.default_rng(42)
    volume = rng.random(shape, dtype=np.float32)

    # The array must be contiguous for the buffer protocol to hand over its
    # bytes without a copy. ascontiguousarray is a no-op when it already is.
    volume = np.ascontiguousarray(volume)

    meta = {
        "dtype": str(volume.dtype),
        "shape": list(volume.shape),
        "spacing": [0.5, 0.5, 1.0],       # units: whatever your application needs
        "kind": "density",
        "checksum": int(volume.sum()),
    }

    print("Sending...")
    t0 = time.monotonic()
    blob.send("scan-volume", volume, meta=meta)
    elapsed = time.monotonic() - t0
    mb = volume.nbytes / 1e6
    print(f"  queued {mb:.0f} MB in {elapsed:.2f}s ({mb / max(elapsed, 1e-9):.0f} MB/s)")

    # Let the chunks drain before the process, and its session: goes away.
    time.sleep(3)
    print("done")
    return 0


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "receive"
    if mode == "send":
        sys.exit(send())
    elif mode == "receive":
        sys.exit(receive())
    else:
        print(__doc__)
        sys.exit(2)
