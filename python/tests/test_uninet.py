"""UniNet Python tests.

Two kinds of test live here. The codec tests are pure and fast. The network
tests start real sessions that discover each other over a real UDP beacon, so
they are slower and given generous timeouts: discovery is a network event, not
a function call, and asserting immediately after join() would test the scheduler.

Every network test uses a realm unique to this process, so a developer running
the demo on the same machine (or a second CI job on the same box) cannot change
the result.

    pytest python/tests -v
"""
from __future__ import annotations

import os
import subprocess
import sys
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
        pytest.skip("peers did not discover each other: network may block UDP 5670")
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
    b.subscribe("app.v1.>", got.append)
    a.publish("app.v1.update.mesh", {"n": 1})
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


def test_blob_sent_from_subscription_handler(net_pair):
    # Regression: publish() from inside a subscription handler used to queue
    # into the network thread's own command pipe, which cannot drain until the
    # callback returns. A Blob is one publish per chunk, so past the pipe's
    # message HWM (~1000) send() blocked forever - the blob never started and
    # nothing was ever received, with no error on either side. It must execute
    # in place instead.
    a, b = net_pair
    cfg = uninet.BlobConfig()
    cfg.chunk_bytes = 8192          # thousands of chunks from a small payload
    tx = uninet.Blob(a, "files", cfg)
    rx = uninet.Blob(b, "files", cfg)

    got = []
    failed = []
    rx.on_received(lambda info, data: got.append((info, data)))
    rx.on_failed(lambda info, why: failed.append(why))

    payload = bytes((i * 131 + 3) & 0xFF for i in range(32 * 1024 * 1024))

    # This handler runs on a's network thread: the exact path that used to hang.
    sent_id = []
    def handler(env):
        sent_id.append(tx.send("callback", payload))
    a.subscribe("trigger.>", handler)

    b.publish("trigger.1", {"kick": True})
    assert wait_until(lambda: got or failed, timeout=60), \
        "transfer from a subscription handler started and finished"
    assert not failed, f"transfer failed: {failed}"
    info, data = got[0]
    assert info.name == "callback"
    assert data == payload
    assert sent_id and sent_id[0], "send() reported success from inside the handler"


def test_blob_send_survives_concurrent_traffic(net_pair):
    # Regression: while one side is committing to a long synchronous send (a
    # Blob from a subscription handler), the OTHER'S inbound event queue used to
    # be able to fill - czmq caps every actor pipe at ~1000 messages, the queue
    # reader is the very thread that is busy streaming the blob, and a full
    # pipe blocks its writer, which stalls the sender of our stream, which
    # blocks the busy thread again: a deadlock no thread can escape, reported
    # nowhere. Traffic in flight must never be able to wedge a transfer.
    a, b = net_pair
    cfg = uninet.BlobConfig()
    cfg.chunk_bytes = 8192          # 16k chunks, far beyond a 1000-message pipe
    tx = uninet.Blob(a, "vol", cfg)
    rx = uninet.Blob(b, "vol", cfg)

    payload = b"\x00" * (128 * 1024 * 1024)

    got = []
    failed = []
    rx.on_received(lambda info, data: got.append(data))
    rx.on_failed(lambda info, why: failed.append(why))

    traffic = {"count": 0}
    a.subscribe("traffic.>", lambda env: traffic.__setitem__("count", traffic["count"] + 1))

    sent = {"done": False}
    def handler(env):
        tx.send("volume", payload)
        sent["done"] = True
    a.subscribe("trigger.>", handler)

    # Keep b's side busy sending for the whole window. Before the fix this
    # filled the receiving transport's internal queue and nothing ever arrived.
    # The thread is stopped and joined BEFORE the test returns: leaving a
    # daemon mid-publish while the fixture tears the sessions down makes the
    # interpreter abort during finalization.
    stop = threading.Event()
    def spam():
        i = 0
        while not stop.is_set():
            b.publish("traffic.x", {"i": i})
            i += 1
    thread = threading.Thread(target=spam)
    thread.start()

    b.publish("trigger.1", {"kick": True})
    assert wait_until(lambda: got or failed, timeout=90), \
        "a transfer finishes even when the network is saturated with other traffic"
    assert not failed, f"transfer failed: {failed}"
    assert got[0] == payload
    assert sent["done"], "the handler-side send reported completion"
    stop.set()
    thread.join(timeout=5)
    assert traffic["count"] > 0, "the competing traffic was not dropped"


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


# ── shutdown ──────────────────────────────────────────────────────────────

def test_close_is_idempotent(net_pair):
    a, _ = net_pair
    a.close()
    a.close()                       # must not raise or crash
    assert not a.open()
    assert not a.connected()


def test_operations_after_close_are_safe(net_pair):
    a, _ = net_pair
    a.close()
    # Nothing here should crash; a closed session is inert, not a trap.
    assert a.peers() == []
    assert a.publish("t.x", {"n": 1}) is False
    assert "Closed" in a.describe()
    a.subscribe("t.>", lambda m: None)


def test_process_exits_cleanly_with_a_live_subscriber():
    """A session still alive at interpreter exit must not abort the process.

    Regression: ZeroMQ's C atexit handler tears down its global context, and a
    session still holding sockets at that point made czmq abort inside
    zsock_set_sndtimeo. It reproduced whenever a subscriber callback kept the
    session reachable, which is the normal way people use this.
    """
    script = """
import time, os, uninet
r = "exitcheck-%d" % os.getpid()
a = uninet.join("A", realm=r)
b = uninet.join("B", realm=r)
got = []
b.subscribe("t.>", lambda m: got.append(m.data))
for _ in range(100):
    if a.peers() and b.peers():
        break
    time.sleep(0.1)
a.publish("t.x", {"n": 1})
for _ in range(60):
    if got:
        break
    time.sleep(0.1)
print("ok", len(got))
"""
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(
        [os.path.dirname(os.path.dirname(uninet.__file__)), env.get("PYTHONPATH", "")]
    )
    proc = subprocess.run([sys.executable, "-c", script], capture_output=True,
                          text=True, timeout=120, env=env)
    # A crash shows up as a negative return code (a signal), not a Python error.
    assert proc.returncode == 0, (
        f"process exited with {proc.returncode}\n"
        f"stdout: {proc.stdout}\nstderr: {proc.stderr[-2000:]}"
    )
    assert "ok" in proc.stdout


# ── discovery without multicast ───────────────────────────────────────────

def test_gossip_discovery_without_multicast():
    """Two nodes find each other over a rendezvous endpoint, no beacon involved.

    This is the path a USB-tethered device or a VPN link takes, where the two
    ends share no broadcast domain.
    """
    r = realm("gossip")
    base = 25730 + (os.getpid() % 200) * 4
    a = uninet.join("Rendezvous", realm=r,
                    gossip_bind=f"tcp://127.0.0.1:{base}",
                    endpoint=f"tcp://127.0.0.1:{base + 1}")
    b = uninet.join("Dialer", realm=r,
                    gossip_connect=f"tcp://127.0.0.1:{base}",
                    endpoint=f"tcp://127.0.0.1:{base + 2}")
    if not (a.connected() and b.connected()):
        pytest.skip("could not bind the gossip endpoints")

    assert wait_until(lambda: a.peers() and b.peers()), "gossip peers did not pair"
    assert a.peers()[0].name == "Dialer"

    got = []
    b.subscribe("g.>", lambda m: got.append(m.data))
    a.publish("g.x", {"over": "gossip"})
    assert wait_until(lambda: got, timeout=15)
    assert got[0]["over"] == "gossip"


# ── regressions for silently-wrong behaviour ──────────────────────────────

def test_numeric_accessors_convert_instead_of_reading_the_wrong_field():
    """Regression: as_f64() on an integer returned 0.0, as_text() returned "".

    The C++ accessors are documented as unchecked and read their own union
    field whatever the kind is. Through the binding that was a silently wrong
    value, and is_number() invited exactly that mistake.
    """
    assert uninet.Cbor.from_value(5).as_f64() == 5.0
    assert uninet.Cbor.from_value(-7).as_int() == -7
    assert uninet.Cbor.f64(2.5).as_int() == 2
    with pytest.raises(TypeError):
        uninet.Cbor.from_value(5).as_text()
    with pytest.raises(TypeError):
        uninet.Cbor.text("x").as_f64()


def test_strided_buffer_is_refused_not_silently_wrong():
    """Regression: a non-contiguous array transmitted the WRONG elements.

    arr[::2] was read as a flat block, so the receiver got arr[0:n/2] with a
    correct-looking byte count and nothing looked wrong at any layer.
    """
    np = pytest.importorskip("numpy")
    r = realm("strided")
    net = uninet.join("s", realm=r)
    blob = uninet.Blob(net, "f")
    with pytest.raises(ValueError, match="contiguous"):
        blob.send("bad", np.arange(64, dtype=np.float64)[::2])
    # The contiguous form of the same data is accepted.
    assert blob.send("good", np.ascontiguousarray(np.arange(64, dtype=np.float64)[::2]))


def test_float32_keeps_its_width_on_the_wire():
    """Regression: every numpy array went through .tolist(), widening f32 to f64."""
    np = pytest.importorskip("numpy")
    a32 = np.arange(2000, dtype=np.float32)
    assert len(uninet.encode({"p": a32})) < len(uninet.encode({"p": a32.astype(np.float64)}))
    assert uninet.decode(uninet.encode({"p": a32}))["p"] == pytest.approx(list(a32))


def test_builders_refuse_a_mismatched_kind():
    """Regression: append() on a non-array accepted the value then dropped it."""
    with pytest.raises(TypeError):
        uninet.Cbor.map().append(1)
    with pytest.raises(TypeError):
        uninet.Cbor.array().set("k", 1)
    # And the builder is chainable when the kind is right.
    m = uninet.Cbor.map()
    m.set("a", 1).set("b", 2)
    assert uninet.to_json(m) == '{"a":1,"b":2}'


def test_missing_map_key_raises_and_len_covers_text():
    """Regression: a missing key returned a null, indistinguishable from a real
    null value; len() on text and bytes was 0."""
    m = uninet.Cbor.from_value({"a": 1, "n": None})
    with pytest.raises(KeyError):
        m["missing"]
    assert m["n"].is_null()                 # a present null still reads as null
    assert m.get("missing", "fallback") == "fallback"
    assert len(uninet.Cbor.text("hello")) == 5
    assert len(uninet.Cbor.from_value(b"abcd")) == 4


def test_publish_to_an_unknown_peer_reports_failure(net_pair):
    """Regression: an addressed publish to a departed peer returned True and the
    message vanished, because Node fell back to a broadcast nobody accepted."""
    a, b = net_pair
    assert a.publish("t.x", {"n": 1}, dst=b.uuid()) is True
    assert a.publish("t.x", {"n": 1}, dst="DEADBEEFDEADBEEFDEADBEEFDEADBEEF") is False


def test_blob_send_reports_failure(net_pair):
    """Regression: an oversized payload was streamed in full to be refused at
    the far end, and the sender was told nothing."""
    a, _ = net_pair
    cfg = uninet.BlobConfig()
    cfg.max_blob_bytes = 1024
    blob = uninet.Blob(a, "f", cfg)
    assert blob.send("too-big", b"x" * 5000) == ""      # refused locally
    assert blob.send("fine", b"x" * 100) != ""


def test_blob_survives_being_destroyed_before_its_session(net_pair):
    """Regression: ~Blob left its subscription installed holding a dangling
    pointer, so the next message was a use-after-free on the network thread."""
    a, b = net_pair
    tx = uninet.Blob(a, "f")
    rx = uninet.Blob(b, "f")
    del rx                                   # the subscription outlives it
    import gc; gc.collect()
    tx.send("after-free", b"payload")        # must not crash the process
    time.sleep(1.0)
