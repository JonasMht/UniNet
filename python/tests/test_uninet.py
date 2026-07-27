"""UniNet Python tests.

Two kinds of test live here. The codec tests are pure and fast. The network
tests start real sessions that discover each other over a real UDP beacon, so
they are slower and given generous timeouts — discovery is a network event, not
a function call, and asserting immediately after join() would test the scheduler.

Every network test uses a realm unique to this process, so a developer running
the demo on the same machine (or a second CI job on the same box) cannot change
the result.

    pytest python/tests -v
"""
from __future__ import annotations

import os
import threading
import time

import pytest

import uninet

# ── helpers ───────────────────────────────────────────────────────────────

DISCOVERY_TIMEOUT = 25.0   # generous: a loaded box can take several seconds


def realm(tag: str) -> str:
    return f"uninet-pytest-{os.getpid()}-{tag}"


def wait_until(predicate, timeout: float = DISCOVERY_TIMEOUT, interval: float = 0.05) -> bool:
    """Poll until `predicate` holds or the timeout passes."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


@pytest.fixture
def net_pair(request):
    """Two sessions in a private realm that have already found each other."""
    tag = request.node.name.replace("[", "-").replace("]", "")
    r = realm(tag)
    a = uninet.join("Node A", role="server", realm=r)
    b = uninet.join("Node B", role="viewer", realm=r)
    if not (a.connected() and b.connected()):
        pytest.skip("no usable network on this machine")
    if not wait_until(lambda: len(a.peers()) == 1 and len(b.peers()) == 1):
        pytest.skip("peers did not discover each other — network may block UDP 5670")
    yield a, b
    del a, b


# ── codec: no network involved ────────────────────────────────────────────

def test_dict_round_trips_through_cbor():
    original = {
        "code": "update",
        "count": 42,
        "ratio": 0.5,
        "enabled": True,
        "missing": None,
        "name": "liver-04",
        "nested": {"a": [1.0, 2.0, 3.0]},
    }
    back = uninet.decode(uninet.encode(original))
    assert back == original


def test_negative_and_large_integers():
    for value in (-1, -1000, 0, 2**31, 2**53):
        assert uninet.decode(uninet.encode({"v": value}))["v"] == value


def test_bytes_round_trip():
    payload = {"blob": b"\x00\x01\xfe\xff"}
    assert uninet.decode(uninet.encode(payload))["blob"] == payload["blob"]


def test_float_list_uses_the_fast_path_and_still_round_trips():
    # A numeric list becomes a contiguous float array on the wire (the mesh fast
    # path) and must still come back as an ordinary list of numbers.
    points = [1.5, 2.5, 3.5, 4.5]
    back = uninet.decode(uninet.encode({"points": points}))
    assert back["points"] == pytest.approx(points)
    assert isinstance(back["points"], list)


def test_large_mesh_payload():
    points = [float(i) * 0.001 for i in range(12288)]   # 4096 vertices
    encoded = uninet.encode({"polydata": {"points": points}})
    back = uninet.decode(encoded)
    assert back["polydata"]["points"] == pytest.approx(points)


def test_json_and_cbor_are_the_same_data_model():
    text = '{"code":"update","n":7,"pts":[1.0,2.0]}'
    from_text = uninet.from_json(text)
    assert from_text == {"code": "update", "n": 7, "pts": [1.0, 2.0]}
    # JSON -> value -> JSON is stable, which is what makes the C#/C++/Python
    # views of one message identical.
    assert uninet.from_json(uninet.to_json(from_text)) == from_text


def test_malformed_input_raises_rather_than_returning_null():
    with pytest.raises(ValueError):
        uninet.from_json("{not json}")
    with pytest.raises(ValueError):
        uninet.decode(b"\xff\xff\xff\xff")


def test_cbor_index_out_of_range_raises():
    value = uninet.Cbor.from_value([1, 2, 3])
    with pytest.raises(IndexError):
        _ = value[99]


def test_unsupported_type_raises_typeerror():
    with pytest.raises(TypeError):
        uninet.encode({"bad": object()})


def test_non_string_key_raises():
    with pytest.raises(TypeError):
        uninet.encode({1: "no"})


def test_compression_enum_is_reachable():
    # "None" is a Python keyword; the enum member must not be named that.
    assert uninet.Compression.NONE is not None
    assert uninet.Compression.ZLIB is not None


# ── network ───────────────────────────────────────────────────────────────

def test_sessions_discover_each_other(net_pair):
    a, b = net_pair
    peers = a.peers()
    assert len(peers) == 1
    assert peers[0].name == "Node B"
    assert peers[0].role == "viewer"
    assert peers[0].address                      # observed, not self-reported
    assert peers[0].uuid == b.uuid()


def test_publish_dict_receive_dict(net_pair):
    a, b = net_pair
    received = []
    b.subscribe("t.>", lambda msg: received.append(msg.data))

    payload = {"code": "update", "points": [1.0, 2.0, 3.0], "n": 5}
    a.publish("t.x", payload)

    assert wait_until(lambda: len(received) == 1, timeout=10)
    assert received[0]["code"] == "update"
    assert received[0]["n"] == 5
    assert received[0]["points"] == pytest.approx([1.0, 2.0, 3.0])


def test_message_exposes_subject_src_and_json(net_pair):
    a, b = net_pair
    got = []
    b.subscribe("t.>", got.append)
    a.publish("t.detail", {"x": 1})

    assert wait_until(lambda: got, timeout=10)
    msg = got[0]
    assert msg.subject == "t.detail"
    assert msg.src == a.uuid()
    assert msg.json() == '{"x":1}'


def test_addressed_message_reaches_only_its_target(request):
    r = realm("addressed")
    a = uninet.join("Sender", realm=r)
    b = uninet.join("Target", realm=r)
    c = uninet.join("Bystander", realm=r)
    if not wait_until(lambda: len(a.peers()) == 2):
        pytest.skip("peers did not discover each other")

    b_got, c_got = [], []
    b.subscribe("t.>", b_got.append)
    c.subscribe("t.>", c_got.append)

    a.publish("t.private", {"secret": True}, dst=b.uuid())
    assert wait_until(lambda: len(b_got) == 1, timeout=10)
    time.sleep(0.5)                       # give a stray copy time to show up
    assert len(c_got) == 0


def test_sender_does_not_receive_its_own_broadcast(net_pair):
    a, _ = net_pair
    own = []
    a.subscribe("t.>", own.append)
    a.publish("t.x", {"n": 1})
    time.sleep(1.0)
    assert own == []


def test_wildcard_subject_matching(net_pair):
    a, b = net_pair
    got = []
    b.subscribe("thermonav.v1.>", got.append)
    a.publish("thermonav.v1.update.mesh", {"n": 1})
    a.publish("other.topic", {"n": 2})

    assert wait_until(lambda: len(got) == 1, timeout=10)
    time.sleep(0.5)
    assert len(got) == 1                  # the non-matching subject stayed out


def test_peer_departure_is_reported(request):
    r = realm("departure")
    a = uninet.join("Stayer", realm=r)
    b = uninet.join("Leaver", realm=r)
    if not wait_until(lambda: len(a.peers()) == 1):
        pytest.skip("peers did not discover each other")

    lost = []
    a.on_peer_lost(lost.append)
    del b                                  # clean shutdown announces the exit
    assert wait_until(lambda: lost, timeout=20)
    assert wait_until(lambda: a.peers() == [], timeout=10)


def test_realms_are_isolated():
    a = uninet.join("Clinical", realm=realm("clinical"))
    b = uninet.join("Developer", realm=realm("dev"))
    time.sleep(3.0)
    assert a.peers() == []
    assert b.peers() == []


def test_publish_from_many_threads(net_pair):
    a, b = net_pair
    received = []
    lock = threading.Lock()

    def collect(msg):
        with lock:
            received.append(msg.data["i"])

    b.subscribe("load.>", collect)

    threads, per_thread = 4, 25

    def worker(t: int):
        for i in range(per_thread):
            a.publish("load.x", {"t": t, "i": i})

    workers = [threading.Thread(target=worker, args=(t,)) for t in range(threads)]
    for w in workers:
        w.start()
    for w in workers:
        w.join()

    assert wait_until(lambda: len(received) == threads * per_thread, timeout=25), (
        f"got {len(received)}/{threads * per_thread}"
    )


def test_raising_handler_does_not_kill_the_process(net_pair):
    a, b = net_pair
    survived = []

    def bad(msg):
        raise RuntimeError("handler blew up on purpose")

    b.subscribe("t.>", bad)
    b.subscribe("t.>", survived.append)

    a.publish("t.x", {"n": 1})
    # The raising handler is reported to stderr and swallowed; the other handler
    # must still be called, and the interpreter must still be alive.
    assert wait_until(lambda: survived, timeout=10)


def test_session_is_a_context_manager():
    r = realm("ctxmgr")
    with uninet.join("Scoped", realm=r) as net:
        assert net.connected()
        assert "Scoped" in net.describe()


def test_describe_is_plain_language(net_pair):
    a, _ = net_pair
    text = a.describe()
    assert "Node A" in text
    assert "device" in text


# ── large payloads (files, volumes, meshes) ─────────────────────────────

def test_blob_round_trip(net_pair):
    a, b = net_pair
    tx = uninet.Blob(a, "files")
    rx = uninet.Blob(b, "files")

    got = []
    progress = []
    rx.on_received(lambda info, data: got.append((info, data)))
    rx.on_progress(lambda info, done: progress.append(done))

    # Deliberately not chunk-aligned: an off-by-one in reassembly shows up here.
    payload = bytes((i * 31 + 7) & 0xFF for i in range(1_500_000))
    tx.send("payload.bin", payload, meta={"kind": "test", "n": 1_500_000})

    assert wait_until(lambda: got, timeout=30)
    info, data = got[0]
    assert info.name == "payload.bin"
    assert info.size == len(payload)
    assert info.src == a.uuid()
    assert info.meta["kind"] == "test"
    assert info.meta["n"] == 1_500_000        # integers stay integers
    assert data == payload                    # byte-for-byte
    assert progress and progress[-1] == len(payload)


def test_blob_numpy_volume_round_trip(net_pair):
    np = pytest.importorskip("numpy")
    a, b = net_pair
    tx = uninet.Blob(a, "vol")
    rx = uninet.Blob(b, "vol")

    got = []
    rx.on_received(lambda info, data: got.append((info, data)))

    volume = np.arange(32 * 32 * 16, dtype=np.float32).reshape(32, 32, 16)
    tx.send("volume", np.ascontiguousarray(volume),
            meta={"dtype": str(volume.dtype), "shape": list(volume.shape)})

    assert wait_until(lambda: got, timeout=30)
    info, data = got[0]
    # The shape must come back as integers, or reshape() raises.
    assert all(isinstance(d, int) for d in info.meta["shape"])
    rebuilt = np.frombuffer(data, dtype=np.dtype(info.meta["dtype"]))
    rebuilt = rebuilt.reshape(info.meta["shape"])
    assert rebuilt.shape == volume.shape
    assert np.array_equal(rebuilt, volume)


def test_blob_file_round_trip(net_pair, tmp_path):
    a, b = net_pair
    tx = uninet.Blob(a, "files")
    rx = uninet.Blob(b, "files")

    src = tmp_path / "case.bin"
    src.write_bytes(b"UniNet file transfer test\n" * 5000)

    got = []
    rx.on_received(lambda info, data: got.append((info, data)))
    tx.send_file(str(src))

    assert wait_until(lambda: got, timeout=30)
    info, data = got[0]
    assert info.name == "case.bin"            # basename, not the full path
    assert data == src.read_bytes()


def test_blob_empty_payload(net_pair):
    a, b = net_pair
    tx = uninet.Blob(a, "files")
    rx = uninet.Blob(b, "files")

    got = []
    rx.on_received(lambda info, data: got.append(data))
    tx.send("nothing", b"")

    assert wait_until(lambda: got, timeout=20)
    assert got[0] == b""


def test_blob_addressed_is_private(request):
    r = realm("blob-dst")
    a = uninet.join("Sender", realm=r)
    b = uninet.join("Target", realm=r)
    c = uninet.join("Bystander", realm=r)
    if not wait_until(lambda: len(a.peers()) == 2):
        pytest.skip("peers did not discover each other")

    tx = uninet.Blob(a, "files")
    target = uninet.Blob(b, "files")
    bystander = uninet.Blob(c, "files")

    got_t, got_b = [], []
    target.on_received(lambda i, d: got_t.append(d))
    bystander.on_received(lambda i, d: got_b.append(d))

    tx.send("private", b"x" * 600_000, dst=b.uuid())
    assert wait_until(lambda: got_t, timeout=25)
    time.sleep(1.0)
    assert got_b == []


def test_blob_rejects_oversized_transfer(net_pair):
    a, b = net_pair
    cfg = uninet.BlobConfig()
    cfg.max_blob_bytes = 1024          # tiny, so the guard is easy to trip
    tx = uninet.Blob(a, "files")
    rx = uninet.Blob(b, "files", cfg)

    failures = []
    got = []
    rx.on_failed(lambda info, why: failures.append(why))
    rx.on_received(lambda info, data: got.append(data))

    tx.send("too-big", b"y" * 50_000)
    assert wait_until(lambda: failures, timeout=20), "the receiver should refuse it"
    assert got == []
    assert "large" in failures[0].lower()


def test_node_survives_a_temporary_transport():
    # Regression: the binding used to keep no reference to the transport, so a
    # temporary was freed while the Node still pointed at it.
    node = uninet.Node("solo", uninet.LoopbackTransport())
    node.connect()
    node.publish("t.x", {"n": 1})          # would be a use-after-free without keep_alive
