#!/usr/bin/env python3
"""UniNet — sending a file to another machine.

    python3 file_transfer.py receive [output-dir]     # terminal 1
    python3 file_transfer.py send <path> [more...]    # terminal 2

No address is configured. The sender finds the receiver and streams the file
with progress at both ends.
"""
import os
import sys
import time

import uninet

SUBJECT = "files"


def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024
    return f"{n:.1f} GB"


def receive(outdir: str) -> int:
    os.makedirs(outdir, exist_ok=True)
    net = uninet.join("File Receiver", role="viewer", app="uninet-examples")
    blob = uninet.Blob(net, SUBJECT)

    def on_progress(info, done):
        pct = 100.0 * done / info.size if info.size else 100.0
        print(f"\r  {info.name}: {pct:5.1f}%  {human(done)}/{human(info.size)}",
              end="", flush=True)

    def on_received(info, data):
        # Never trust a remote-supplied filename: basename() stops "../../etc/x"
        # from escaping the output directory.
        safe = os.path.basename(info.name) or "unnamed"
        path = os.path.join(outdir, safe)
        with open(path, "wb") as fh:
            fh.write(data)
        print(f"\r  {info.name}: saved to {path} ({human(len(data))})          ")
        if info.meta and info.meta.get("sha256"):
            import hashlib
            got = hashlib.sha256(data).hexdigest()
            ok = "OK" if got == info.meta["sha256"] else "MISMATCH"
            print(f"    sha256 {ok}")

    blob.on_progress(on_progress)
    blob.on_received(on_received)
    blob.on_failed(lambda info, why: print(f"\r  {info.name} failed: {why}"))

    print(net.describe())
    print(f"Saving incoming files to {outdir}. Ctrl+C to stop.\n")
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


def send(paths) -> int:
    import hashlib

    net = uninet.join("File Sender", role="server", app="uninet-examples")
    blob = uninet.Blob(net, SUBJECT)

    print(net.describe())
    print("Looking for a receiver…")
    for _ in range(200):
        if net.peers():
            break
        time.sleep(0.1)
    if not net.peers():
        print("No receiver found. Start 'file_transfer.py receive' first.")
        return 1
    print(f"Found {net.peers()[0].name} at {net.peers()[0].endpoint}\n")

    for path in paths:
        if not os.path.isfile(path):
            print(f"  skipping {path}: not a file")
            continue
        with open(path, "rb") as fh:
            digest = hashlib.sha256(fh.read()).hexdigest()
        size = os.path.getsize(path)
        print(f"  sending {os.path.basename(path)} ({human(size)})")
        blob.send_file(path, meta={"sha256": digest, "size": size})

    # Let the chunks drain before the session goes away.
    time.sleep(3)
    print("done")
    return 0


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "receive"
    if mode == "send" and len(sys.argv) > 2:
        sys.exit(send(sys.argv[2:]))
    elif mode == "receive":
        sys.exit(receive(sys.argv[2] if len(sys.argv) > 2 else "./received"))
    else:
        print(__doc__)
        sys.exit(2)
