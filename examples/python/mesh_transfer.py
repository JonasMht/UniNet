#!/usr/bin/env python3
"""UniNet: streaming a 3D mesh, the way a live navigation view does.

A mesh is small enough to send as an ordinary message, so this uses publish()
rather than Blob. The point it demonstrates is the float fast path: a numeric
list is stored contiguously on the wire, so a 4096-vertex mesh costs one bulk
write rather than a node per coordinate.

    python3 mesh_transfer.py receive          # terminal 1
    python3 mesh_transfer.py send             # terminal 2

Rule of thumb: publish() for anything up to a few MB, Blob for anything larger
or anything you want progress on.
"""
import sys
import time

import numpy as np

import uninet

SUBJECT = "scene.v1.mesh"


def make_mesh(n_verts: int):
    """A sphere-ish surface: points plus triangle indices."""
    rng = np.random.default_rng(7)
    phi = rng.uniform(0, np.pi, n_verts)
    theta = rng.uniform(0, 2 * np.pi, n_verts)
    r = 50.0
    pts = np.stack([r * np.sin(phi) * np.cos(theta),
                    r * np.sin(phi) * np.sin(theta),
                    r * np.cos(phi)], axis=1).astype(np.float32)
    tris = rng.integers(0, n_verts, size=(n_verts, 3), dtype=np.int64)
    return pts, tris


def receive() -> int:
    net = uninet.join("Mesh Viewer", role="viewer", app="uninet-examples")

    def on_mesh(msg):
        d = msg.data
        # Numeric lists come back as lists of floats; numpy rebuilds the shape.
        pts = np.asarray(d["points"], dtype=np.float32).reshape(-1, 3)
        tris = np.asarray(d["polys"], dtype=np.int64).reshape(-1, 3)
        print(f"  mesh '{d['name']}': {len(pts)} verts, {len(tris)} tris, "
              f"frame {d['frame']}, bounds "
              f"[{pts.min():.1f}, {pts.max():.1f}]")

    net.subscribe(SUBJECT, on_mesh)

    print(net.describe())
    print(f"Waiting for meshes on '{SUBJECT}'. Ctrl+C to stop.\n")
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\nstopped")
    return 0


def send() -> int:
    net = uninet.join("Mesh Source", role="server", app="uninet-examples")

    print(net.describe())
    print("Looking for a viewer...")
    for _ in range(200):
        if net.peers():
            break
        time.sleep(0.1)
    if not net.peers():
        print("No viewer found. Start 'mesh_transfer.py receive' first.")
        return 1
    print(f"Found {net.peers()[0].name}\n")

    pts, tris = make_mesh(4096)
    print(f"Streaming a {len(pts)}-vertex mesh at 20 Hz (Ctrl+C to stop)...")

    frame = 0
    t0 = time.monotonic()
    try:
        while True:
            frame += 1
            # Wobble the mesh so each frame differs, as a live surface would.
            wobble = pts * (1.0 + 0.01 * np.sin(frame * 0.1))
            net.publish(SUBJECT, {
                "name": "surface-01",
                "frame": frame,
                "points": wobble.ravel(),      # numpy -> contiguous float array
                "polys": tris.ravel(),
                "transform": np.eye(4, dtype=np.float64).ravel(),
            })
            if frame % 20 == 0:
                rate = frame / (time.monotonic() - t0)
                print(f"\r  {frame} frames, {rate:.1f} Hz", end="", flush=True)
            time.sleep(0.05)                   # 20 Hz
    except KeyboardInterrupt:
        print("\nstopped")
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
