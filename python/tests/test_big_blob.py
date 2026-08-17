"""One genuinely big blob, end to end, over the real network.

The transport regressions prove that many small chunks cannot wedge a
transfer, but nothing in the suite moves a blob that is large in its own
right: hundreds of megabytes with the real 256 KiB default chunking, sent
from inside a subscription handler - the exact pattern the Slicer module
uses. This is the test for "it works at the size I actually use".

Memory is honest too: the sender keeps the whole payload and the receiver
reassembles it, so the process needs a few times the blob size. Tune it:

    UNINET_BIG_BLOB_MB=1024            how big (default 512, 0 forces 512)
    UNINET_BIG_BLOB_TRAFFIC=0          disable the competing traffic pump
    UNINET_BIG_BLOB_VERBOSE=1          print progress and timings

    pytest python/tests/test_big_blob.py -v -s
"""
from __future__ import annotations

import hashlib
import os
import threading
import time

import numpy as np
import pytest

import uninet

SIZE_MB = int(os.environ.get("UNINET_BIG_BLOB_MB", "512") or "512")
if SIZE_MB <= 0:
    SIZE_MB = 512
VERBOSE = os.environ.get("UNINET_BIG_BLOB_VERBOSE", "") not in ("", "0")
TRAFFIC = os.environ.get("UNINET_BIG_BLOB_TRAFFIC", "1") not in ("", "0")
DISCOVERY_TIMEOUT = 25.0
RECEIVE_TIMEOUT = 300.0

pytestmark = pytest.mark.heavy


def _digest(buffer, block: int = 8 * 1024 * 1024) -> str:
    """A modest-memory fingerprint of a large buffer, streamed in blocks."""
    digest = hashlib.blake2b()
    view = memoryview(buffer)
    for start in range(0, len(view), block):
        digest.update(view[start:start + block])
    return digest.hexdigest()


def _realm(tag: str) -> str:
    return f"uninet-bigblob-{os.getpid()}-{tag}"


@pytest.fixture(scope="module")
def big_pair():
    """Two sessions in a private realm that have already found each other."""
    realm = _realm(f"m{os.getpid()}")
    a = uninet.join("Big Sender", role="server", realm=realm)
    b = uninet.join("Big Receiver", role="viewer", realm=realm)
    if not (a.connected() and b.connected()):
        pytest.skip("no usable network on this machine")
    deadline = time.monotonic() + DISCOVERY_TIMEOUT
    while time.monotonic() < deadline and not (
            len(a.peers()) == 1 and len(b.peers()) == 1):
        time.sleep(0.05)
    if not (len(a.peers()) == 1 and len(b.peers()) == 1):
        pytest.skip("peers did not discover each other: network may block UDP 5670")
    yield a, b
    del a, b


def test_big_blob_from_subscription_handler(big_pair):
    # A blob large in its own right - not many small chunks from a small blob -
    # sent from a network-thread handler with the shipped defaults, while the
    # other side keeps the network busy. This is the Slicer module's pattern,
    # at the size its volumes would be.
    a, b = big_pair
    tx = uninet.Blob(a, "volume")          # default 256 KiB chunks
    rx = uninet.Blob(b, "volume")

    got = []
    failed = []
    progress = []
    rx.on_received(lambda info, data: got.append((info, data)))
    rx.on_failed(lambda info, why: failed.append(why))
    rx.on_progress(lambda info, done: progress.append(done))

    try:
        payload = np.random.default_rng(20260817).random(
            SIZE_MB * 1024 * 1024 // 4, dtype=np.float32)
    except MemoryError:                     # noqa: PERF203
        pytest.skip(f"{SIZE_MB} MiB payload does not fit in this process")
    expected = _digest(payload)
    size_mb = payload.nbytes / (1024 * 1024)

    traffic = {"count": 0}
    if TRAFFIC:
        a.subscribe("traffic.>",
                    lambda env: traffic.__setitem__("count", traffic["count"] + 1))

    started = {"id": None}
    def handler(env):                       # runs on a's network thread
        started["id"] = tx.send("volume", payload)
    a.subscribe("trigger.>", handler)

    stop = threading.Event()
    thread = None
    if TRAFFIC:
        def spam():
            i = 0
            while not stop.is_set():
                b.publish("traffic.x", {"i": i})
                i += 1
        thread = threading.Thread(target=spam)
        thread.start()

    t0 = time.monotonic()
    b.publish("trigger.1", {"kick": True})
    deadline = time.monotonic() + RECEIVE_TIMEOUT
    while not (got or failed) and time.monotonic() < deadline:
        time.sleep(0.1)

    if TRAFFIC:
        stop.set()
        thread.join(timeout=5)

    assert failed or got, \
        f"a {size_mb:.0f} MiB blob did not finish in {RECEIVE_TIMEOUT:.0f}s"
    assert not failed, f"transfer failed: {failed}"
    assert started["id"], "send() inside the subscription handler reported failure"

    info, data = got[0]
    assert info.name == "volume"
    assert len(data) == payload.nbytes, \
        f"{len(data)} bytes arrived for a {payload.nbytes}-byte payload"

    # Sender and receiver must hold the same bytes: a digest, computed in
    # blocks, so this assertion does not need another big copy.
    assert _digest(data) == expected, "received bytes differ from what was sent"

    if progress:
        assert max(progress) == payload.nbytes, "progress never reached 100%"
    if TRAFFIC:
        assert traffic["count"] > 0, "the competing traffic was not delivered"
    if VERBOSE:
        print(f"\n  {size_mb:.0f} MiB from network-thread handler: "
              f"{time.monotonic() - t0:.2f}s, {len(progress)} progress reports, "
              f"traffic {traffic['count']}")
