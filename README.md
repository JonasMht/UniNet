# UniNet

**Devices on a network find each other and talk. Nobody configures anything.**

```cpp
auto net = uninet::Session::join("OR Headset");
net->subscribe("domain.>", [](auto& msg) { /* ... */ });
net->publish("domain.D1", data);
```

That is the complete setup. No IP address, no port, no broker to install, no
server to run, no configuration file. Start the same program on another machine
on the same network and the two find each other in about a second.

The same three lines, in every language UniNet supports:

<table>
<tr><th>C++</th><th>Python</th><th>C#</th></tr>
<tr valign="top"><td>

```cpp
auto net = Session::join("Server");

net->subscribe("domain.>",
  [](const Envelope& m) {
    print(to_json(m.data));
  });

net->publish("domain.D1", data);
```
</td><td>

```python
net = uninet.join("Slicer")

net.subscribe("domain.>",
    lambda m: print(m.data))


net.publish("domain.D1",
            {"code": "update"})
```
</td><td>

```csharp
var net = Session.Join("MR");

net.Subscribe("domain.>",
  m => Debug.Log(m.Json));


net.Publish("domain.D1",
            "{\"code\":\"update\"}");
```
</td></tr>
</table>

A dict published from Python arrives as a `Cbor` map in C++ and as JSON in C#.
One codec, one wire format, three languages — see [Data](#data-json-in-cbor-on-the-wire-json-out).

---

## Contents

- [Why it exists](#why-it-exists)
- [Install](#install)
- [Quick start: C++](#quick-start-c) · [Python](#quick-start-python) · [C#](#quick-start-c-1)
- [Data: JSON in, CBOR on the wire, JSON out](#data-json-in-cbor-on-the-wire-json-out)
- [Large payloads: files, volumes, meshes](#large-payloads-files-volumes-meshes)
- [Finding devices](#finding-devices)
- [Realms: keeping setups apart](#realms-keeping-setups-apart)
- [Unity / Meta Quest](#unity--meta-quest)
- [3D Slicer](#3d-slicer)
- [Command-line tools](#command-line-tools)
- [Performance](#performance)
- [How it works](#how-it-works)
- [Testing](#testing)
- [Troubleshooting](#troubleshooting)

---

## Why it exists

Three applications — a C++ server, a Python module inside 3D Slicer, and a
C#/Unity app on a Meta Quest — needed to talk to each other. Each had
re-implemented the same messaging code, and all three had the broker's IP
address `10.0.0.10:4222` compiled into their source. Changing it meant editing
three codebases in three languages and rebuilding an APK for the headset.

UniNet removes the address and the broker at the same time. Devices announce
themselves on the local network and connect directly to each other. There is
nothing to install on a server, because there is no server.

**What UniNet gives you**

| | |
|---|---|
| **Zero configuration** | Devices discover each other. No addresses, anywhere. |
| **No broker** | Peer-to-peer. Nothing to install, start, or keep running. |
| **Three languages** | C++, Python, C# — one data model, identical bytes on the wire. |
| **Presence** | Know who is on the network, and when someone joins or leaves. |
| **JSON or CBOR** | Write JSON, send compact binary, read JSON. Your choice per call. |
| **Fast** | 18k messages/s at 60 KB each; 2.3 GB/s at 240 KB. |

---

## Install

### Prerequisites

UniNet needs **ZeroMQ/Zyre** (the peer-to-peer layer), **zlib**, and optionally
**liblz4**. If Zyre is not installed, the build fetches and compiles it
automatically — so on most machines you can skip straight to the build.

| OS | install prerequisites |
|---|---|
| **Ubuntu/Debian** | `sudo apt install build-essential cmake pkg-config zlib1g-dev liblz4-dev libzyre-dev` |
| **Fedora** | `sudo dnf install gcc-c++ cmake pkgconf-pkg-config zlib-devel lz4-devel zyre-devel` |
| **Arch** | `sudo pacman -S base-devel cmake pkgconf zlib lz4 zyre` |
| **macOS** | `brew install cmake pkg-config zlib lz4 zyre` |
| **Windows** | `scripts/bootstrap.ps1` (clones vcpkg for the dependencies); needs Git, CMake, and VS 2022's "Desktop development with C++" workload |

> If `libzyre-dev` is unavailable, install nothing extra — CMake will build
> libzmq + czmq + zyre from source on first configure. It adds a few minutes to
> the first build and nothing after that.

### Build

```bash
git clone <this repo> && cd UniNet
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUNINET_BUILD_CABI=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

That produces:

| artifact | what it is |
|---|---|
| `build/libuninet.a` | the C++ library |
| `build/libuninet_c.so` | the C ABI, for C# / Unity / any FFI |
| `build/uninet-discover` | CLI: what is on my network? |
| `build/uninet-demo` | the demo (see [Command-line tools](#command-line-tools)) |
| `build/uninet-benchmark` | end-to-end network benchmark |

**Python:**

```bash
pip install .
```

**CMake options** (all optional):

| option | default | meaning |
|---|---|---|
| `UNINET_BUILD_CABI` | OFF | build `libuninet_c` for C#/FFI |
| `UNINET_BUILD_PYTHON` | OFF | build the Python extension (`pip install` sets it) |
| `UNINET_LZ4` | ON | LZ4 compression tier, auto-detected |
| `UNINET_SYSTEM_ZYRE` | ON | use an installed zyre; OFF forces a source build |

---

## Quick start: C++

```cpp
#include "uninet/session.h"
#include "uninet/json.h"

int main() {
    // The whole setup. The name is what other devices will show for this one.
    auto net = uninet::Session::join("Navigation Server");

    // Who else is here — now, and as devices come and go.
    net->on_peer_found([](const uninet::Peer& p) {
        printf("found %s\n", p.describe().c_str());
    });
    net->on_peer_lost([](const uninet::Peer& p) {
        printf("lost %s\n", p.name.c_str());
    });

    // Receive. A subject ending in ">" matches everything below it.
    net->subscribe("domain.>", [](const uninet::Envelope& msg) {
        printf("%s: %s\n", msg.subject.c_str(), uninet::to_json(msg.data).c_str());
    });

    // Send — to everyone…
    uninet::Cbor data = uninet::Cbor::map();
    data.set("code", uninet::Cbor::text("update"));
    net->publish("domain.D1", data);

    // …or from JSON, whichever is more convenient at the call site.
    net->publish_json("domain.D1", R"({"code":"update","n":42})");

    // …or privately, to one device.
    for (const auto& p : net->peers())
        if (p.role() == "headset")
            net->publish("domain.D1", data, p.uuid);
}
```

Link with CMake:

```cmake
add_subdirectory(UniNet)
target_link_libraries(your_app PRIVATE uninet)
```

Or, after `cmake --install`:

```cmake
find_package(UniNet REQUIRED)
target_link_libraries(your_app PRIVATE uninet)
```

**Configuration**, when you need it — the defaults are right for a single
network:

```cpp
uninet::SessionConfig cfg;
cfg.role  = "server";          // free-form label shown to other devices
cfg.app   = "ThermoNavServer"; // owning application
cfg.realm = "or-3";            // see Realms below
cfg.iface = "eth0";            // only on a machine with several networks
auto net = uninet::Session::join("Navigation Server", cfg);
```

---

## Quick start: Python

```python
import uninet

net = uninet.join("Slicer Viewer", role="viewer")

net.on_peer_found(lambda p: print("found", p.name, p.address, p.role))
net.on_peer_lost(lambda p: print("lost", p.name))

# The payload is a plain dict, in and out.
net.subscribe("domain.>", lambda msg: print(msg.subject, msg.data))

net.publish("domain.D1", {
    "code": "update",
    "points": [1.0, 2.0, 3.0],       # numeric lists take the fast binary path
    "nested": {"case": "liver-04"},
})

for peer in net.peers():
    print(peer.name, peer.host, peer.role)
```

Send privately to one device:

```python
for p in net.peers():
    if p.role == "headset":
        net.publish("domain.D1", {"code": "update"}, dst=p.uuid)
```

`Session` is also a context manager, which announces the departure promptly:

```python
with uninet.join("Tool") as net:
    net.publish("t.x", {"hello": True})
```

Profiling a hot loop:

```python
with uninet.profiling():
    for _ in range(1000):
        net.publish("t.x", payload)
# prints a per-operation breakdown sorted by total time
```

---

## Quick start: C#

```csharp
using UniNet;

using var net = Session.Join("MR Viewer", role: "headset");

net.PeerFound += p => Console.WriteLine($"found {p.Name} at {p.Host} ({p.Role})");
net.PeerLost  += p => Console.WriteLine($"lost {p.Name}");

net.Subscribe("domain.>", msg => Console.WriteLine($"{msg.Subject}: {msg.Json}"));

net.Publish("domain.D1", "{\"code\":\"update\",\"n\":42}");

foreach (var p in net.Peers())
    Console.WriteLine($"{p.Name}  {p.Host}  {p.Role}");
```

**In Unity, one extra line is required.** Messages arrive on a background
network thread, and touching the Unity API from there crashes the player. By
default UniNet queues events for you; drain the queue from `Update()`:

```csharp
void Update() => net.Update();     // delivers callbacks on the main thread
```

Outside Unity (a console app or service) pass `marshalToCaller: false` to get
events immediately on the network thread instead.

Place the native library where .NET can find it: next to the executable, or in
`Assets/Plugins/<platform>/` for Unity.

---

## Data: JSON in, CBOR on the wire, JSON out

UniNet sends **CBOR** — a compact binary format that carries typed values and
stores float arrays as contiguous blocks, so a 4096-vertex mesh costs no
per-number overhead.

You never have to write CBOR. Use whatever your language makes natural:

| language | what you write | what goes on the wire |
|---|---|---|
| Python | `{"code": "update", "pts": [1.0, 2.0]}` | CBOR |
| C# | `"{\"code\":\"update\",\"pts\":[1.0,2.0]}"` | CBOR |
| C++ | `Cbor::map().set(...)` or `publish_json(...)` | CBOR |

All three produce **the same bytes**, and each peer reads them in its own idiom.
That is what makes cross-language work stop being a schema-mirroring exercise.

Converting explicitly, in any language:

```cpp
uninet::Cbor v = uninet::from_json(R"({"a":1})");
std::string  s = uninet::to_json(v);
```
```python
v = uninet.from_json('{"a":1}');  s = uninet.to_json(v)
```
```csharp
byte[] cbor = Session.JsonToCbor("{\"a\":1}");
string json = Session.CborToJson(cbor);
```

**Where JSON is a smaller type system than CBOR**, and what UniNet does:

| CBOR | rendered as JSON |
|---|---|
| byte string | base64 text |
| float array | ordinary JSON array (on one line, even when pretty-printing — a 12288-element array on 12288 lines helps nobody) |
| integer > 2⁵³ | survives CBOR exactly; loses precision in JSON consumers |
| NaN / Infinity | `null` (JSON has no representation for them) |

---

## Large payloads: files, volumes, meshes

`publish()` sends a message: it arrives as one unit, and both ends hold it whole
in memory. That is the right tool up to a few megabytes.

A 200 MB CT volume or a case file is not a message. `Blob` chunks it, streams
it, reassembles it on the far side, and reports progress at both ends:

```python
blob = uninet.Blob(net, "volumes")

# receiving
blob.on_progress(lambda info, done: print(f"{100*done/info.size:.0f}%"))
blob.on_received(lambda info, data: save(info.name, data))

# sending — metadata travels with the payload
blob.send("patient-volume", volume, meta={
    "dtype": str(volume.dtype),
    "shape": list(volume.shape),
    "spacing": [0.5, 0.5, 1.0],
})
blob.send_file("/path/to/case.zip")
```

```cpp
uninet::Blob blob(*net, "volumes");
blob.on_received([](const uninet::BlobInfo& info, const uninet::Bytes& data) {
    save(info.name, data);
});
blob.send_file("/path/to/case.zip");
```

**Rule of thumb:** `publish()` for anything up to a few MB you want as one
message; `Blob` for anything larger, or anything you want a progress bar on.

Because the metadata rides with the payload, a typed transfer needs no side
channel and no schema agreed in advance:

```python
volume = np.frombuffer(data, dtype=np.dtype(info.meta["dtype"]))
volume = volume.reshape(info.meta["shape"])
```

Reassembly is bounded on purpose. A peer on the LAN is unauthenticated, so a
transfer that stalls is dropped, and there are hard caps on size, on concurrent
transfers, and on total bytes in flight (`BlobConfig`). Raise them deliberately.

> Call `np.ascontiguousarray(a)` before sending a numpy array — a sliced or
> transposed array is not contiguous, and the buffer protocol needs it to be.

See [`examples/`](examples/) for complete, runnable versions of all of this.

---

## Finding devices

Every device advertises a name, and optionally a role, an app, and any headers
you choose. All of it arrives with the discovery beacon, so a peer list is
complete the moment a device appears — no follow-up query.

```cpp
for (const uninet::Peer& p : net->peers()) {
    p.uuid;       // address for a private message
    p.name;       // "OR Headset"
    p.address;    // "tcp://192.168.1.31:35001" — observed, not self-reported
    p.role();     // "headset"
    p.app();      // "ThermoNavMR"
    p.header("anything-you-set");
}
```

`on_peer_found` also replays the devices already present, so registration order
never changes what you see.

> **`address` is the address the connection actually came from**, not one the
> peer claims. A device's own idea of its address is wrong behind NAT and
> forgeable everywhere.

---

## Realms: keeping setups apart

Devices only see devices in the **same realm**. It is the one setting that ever
needs changing, and it exists for two situations:

- Two independent setups sharing one hospital network.
- A developer's laptop that must not join a live clinical session.

```cpp
cfg.realm = "or-3";                          // C++
```
```python
uninet.join("Tool", realm="or-3")            # Python
```
```csharp
Session.Join("Tool", realm: "or-3");         // C#
```

Realms isolate both messaging and the peer list — a device in another realm is
invisible, not merely unreachable.

---

## Unity / Meta Quest

**1. Build the native library for Android ARM64** and place it at
`Assets/Plugins/Android/libs/arm64-v8a/libuninet_c.so`.

**2. Add the C# sources.** Copy `csharp/UniNet/*.cs` into `Assets/Plugins/UniNet/`,
or reference the built assembly.

**3. Add the Wi-Fi multicast permission — this is not optional.**

Android's Wi-Fi stack **silently drops multicast and subnet-broadcast frames**
unless the app holds a `WifiManager.MulticastLock`. Without it the headset
receives no discovery traffic at all: it will be invisible to every other device
and blind to all of them, with no error message anywhere.

In `Assets/Plugins/Android/AndroidManifest.xml`:

```xml
<uses-permission android:name="android.permission.INTERNET"/>
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE"/>
<uses-permission android:name="android.permission.ACCESS_WIFI_STATE"/>
<uses-permission android:name="android.permission.CHANGE_WIFI_MULTICAST_STATE"/>
```

It is a normal install-time permission — no runtime request needed.

**4. Acquire the lock before joining, release it on teardown.** A ready-made
helper lives at `docs/unity/UniNetMulticastLock.cs`; copy it into your project.

**5. Wire it into a MonoBehaviour:**

```csharp
using UnityEngine;
using UniNet;

public class UniNetBehaviour : MonoBehaviour
{
    private Session _net;

    void Start()
    {
        UniNetMulticastLock.Acquire();          // must come before Join
        _net = Session.Join("MR Viewer", role: "headset", app: "ThermoNavMR");

        _net.PeerFound += p => Debug.Log($"UniNet: found {p.Name} at {p.Host}");
        _net.Subscribe("domain.>", OnMessage);
    }

    // Called on the MAIN thread, because Update() drains the queue.
    void OnMessage(Message msg) => Debug.Log($"{msg.Subject}: {msg.Json}");

    void Update() => _net?.Update();            // required

    void OnDestroy()
    {
        _net?.Dispose();
        UniNetMulticastLock.Release();
    }
}
```

**Verify the permission shipped**, after your next build:

```bash
aapt dump permissions your.apk | grep MULTICAST
```

> **Ethernet or USB tethering on the headset:** the multicast lock applies to
> Wi-Fi only. On a non-Wi-Fi interface the lock is harmless but does nothing —
> discovery works because those interfaces do not filter multicast.

---

## 3D Slicer

Slicer ships its own Python. The extension module must be built against **that**
interpreter, not the system one.

```bash
# Point CMake at Slicer's Python (adjust the path to your install)
SLICER_PY=/opt/Slicer/bin/PythonSlicer

cmake -S . -B build-slicer -DCMAKE_BUILD_TYPE=Release \
      -DUNINET_BUILD_PYTHON=ON \
      -DPython3_EXECUTABLE=$($SLICER_PY -c "import sys; print(sys.executable)") \
      -Dpybind11_DIR=$($SLICER_PY -m pybind11 --cmakedir)
cmake --build build-slicer -j
```

Copy the resulting extension and package next to Slicer's `site-packages`:

```bash
SITE=$($SLICER_PY -c "import site; print(site.getsitepackages()[0])")
cp -r python/uninet "$SITE/"
```

Then, in your Slicer module:

```python
import uninet

class MyModuleLogic:
    def __init__(self):
        self.net = uninet.join("Slicer Viewer", role="viewer", app="ThermoNavSlicer")
        self.net.subscribe("thermonav.v1.>", self.on_message)
        self.net.on_peer_found(self.on_peer)

    def on_message(self, msg):
        # Called on UniNet's network thread. Slicer's VTK/Qt objects are NOT
        # thread-safe — hop to the main thread before touching the scene.
        import qt
        qt.QTimer.singleShot(0, lambda: self.apply(msg.data))

    def apply(self, data):
        if data.get("code") == "update":
            ...   # safe here: this runs on Slicer's main thread

    def on_peer(self, peer):
        print(f"UniNet: {peer.name} ({peer.role}) at {peer.host}")
```

> **The threading rule is the same as Unity's**: callbacks arrive on a
> background thread. Marshal to the main thread before touching VTK, Qt, or the
> MRML scene. `qt.QTimer.singleShot(0, fn)` is the idiomatic way in Slicer.

A device list widget, since Slicer modules usually want one:

```python
def refresh_device_list(self):
    self.deviceCombo.clear()
    for p in self.net.peers():
        self.deviceCombo.addItem(f"{p.name} — {p.role} ({p.host})", p.uuid)
```

---

## Command-line tools

### `uninet-discover` — what is on my network?

The tool to run when someone says "the headset can't see the server".

```bash
uninet-discover              # live view: devices arriving and leaving
uninet-discover --once       # one snapshot, then exit
```

```
DEVICE                     ADDRESS          ROLE         APP
Navigation Server          192.168.1.10     server       ThermoNavServer
OR Headset                 192.168.1.31     headset      ThermoNavMR
Planning Laptop            192.168.1.24     viewer       ThermoNavSlicer

3 devices.
```

When it finds nothing, it says what to check — in order, in plain language.

| flag | meaning |
|---|---|
| `--once` | one snapshot instead of a live view |
| `--timeout <s>` | how long `--once` listens (default 3) |
| `--realm <name>` | only show devices in this realm |
| `--interface <n>` | which network to look on (`eth0`, or an IP) |
| `--version` | the ZeroMQ/Zyre versions in use |

### `uninet-demo` — two devices talking, with nothing configured

```bash
uninet-demo "Planning Laptop"
uninet-demo "OR Headset" --role headset      # another terminal, or another machine
```

Each prints the other arriving, then they exchange messages. Nobody types an
address. `scripts/demo.sh` runs three at once.

### `uninet-file-transfer` — send a file, no address needed

```bash
uninet-file-transfer receive ./incoming     # one machine
uninet-file-transfer send report.pdf        # another
```

### `uninet-benchmark`

```bash
uninet-benchmark 300      # 300 messages per payload size
```

Measures cold-start discovery and end-to-end throughput, appending each run to
`uninet_network_bench.csv` so runs accumulate.

---

## Performance

Measured with `uninet-benchmark` on a 32-core Linux box, two sessions exchanging
mesh payloads over the full stack — encode → compress → frame → TCP → unframe →
decompress → decode → dispatch:

| payload | size on the wire | messages/s | throughput | delivered |
|---|---|---|---|---|
| 512 verts | 7.7 KB | **38,115** | 280 MB/s | 300/300 |
| **4096 verts** (the live MR mesh) | 61 KB | **18,503** | 1,085 MB/s | 300/300 |
| 16384 verts | 246 KB | **9,777** | 2,292 MB/s | 300/300 |

**Cold-start discovery: 2 ms** for two processes on one machine.

> **On a real network, expect discovery to take about a second**, not two
> milliseconds. ZRE's beacon interval and the Wi-Fi association dominate, and the
> 2 ms figure only reflects a same-host loopback. Treat ~1 s as the number to
> design around, and the 2 ms as a floor.

Codec-level numbers (encode/compress/frame in isolation) come from the separate
`benchmark` target and are logged to `uninet_bench_log.csv`.

**What this means in practice:** the MR peer streams at ~20 Hz. UniNet sustains
roughly 900× that rate at the same payload size, so the protocol layer is not
the bottleneck at any realistic rate.

---

## How it works

```
your code
    │
    ├── Session ................ join, publish, subscribe, peers
    │       │
    │       ├── Node ........... envelope, dst filter, subject matching
    │       │      └── codec ... CBOR + compression (none / zlib / LZ4)
    │       │
    │       └── ZyreTransport .. discovery + peer-to-peer delivery
    │                  │
    │                  └── ZeroMQ / Zyre (ZRE, RFC 36/43)
    │                          UDP beacon on :5670 → direct TCP between peers
    │
    ├── Blob ................... chunked transfer for files / volumes / meshes
    └── JSON bridge ............ from_json / to_json
```

**Source layout**

```
include/uninet/   session.h  ← start here
                  peer.h · zyre_transport.h · blob.h · json.h
                  node.h · transport.h · loopback.h
                  cbor.h · codec.h · types.h · profiler.h · cabi.h
src/              one .cpp per header
python/           bindings.cpp (pybind11) · uninet/ (package) · tests/
csharp/           UniNet/ (Session.cs, Native.cs) · UniNetDemo/
examples/         demo.cpp · file_transfer.cpp · python/ — see examples/README.md
tests/            test_roundtrip (codec) · test_network · test_cabi (C)
                  benchmark (codec) · benchmark_network (end-to-end)
                  interop/ — the three-language interop participants
                  docker/  — Linux and Windows-cross test images
tools/            uninet_discover.cpp
scripts/          test-all.sh · test-interop.sh · demo.sh · bootstrap.{sh,ps1}
docs/             PROTOCOL.md · unity/UniNetMulticastLock.cs
```

**Discovery** is ZRE's UDP beacon: each node broadcasts its presence on the
local link, peers hear it and open a direct TCP connection. Beacons carry a hop
limit of 1, so they never cross a router — discovery cannot leak into the rest of
a hospital network.

**Delivery** is peer-to-peer TCP. A broadcast is a `SHOUT` to the realm group; an
addressed message is a `WHISPER` to one peer, which is a genuine unicast rather
than a broadcast everyone else filters. Echo suppression is free — ZRE never
delivers a node its own broadcast.

**The payload** is UniNet's own CBOR envelope, carrying the subject, the sender's
uuid, and your data, with routing in a clear header before the compressed body
so a receiver can filter without decompressing. See
[`docs/PROTOCOL.md`](docs/PROTOCOL.md).

**Licence note:** Zyre and czmq are MPL-2.0, libzmq is MPL-2.0. These are
file-level copyleft: you can link them into a proprietary application and ship
it; only modifications to *their* source files must be published. UniNet itself
is MIT.

---

## Testing

```bash
./scripts/test-all.sh              # everything runnable natively
./scripts/test-all.sh --docker     # plus cross-platform and cross-language
```

Or run a suite directly:

```bash
ctest --test-dir build --output-on-failure     # C++ core, network, C ABI
PYTHONPATH=python pytest python/tests -v       # Python
./scripts/test-interop.sh                      # C++ <-> Python <-> C#
```

| suite | what it covers |
|---|---|
| `test_roundtrip` | CBOR round-trips, compression, framing, hostile frames |
| `test_network` | real discovery, departure, broadcast, unicast, realm isolation, concurrent publish/subscribe, large-payload transfer |
| `test_cabi` | the C ABI compiled **as C** — the exact path C#/Unity takes, including UTF-8, null-safety and pointer lifetimes |
| `python/tests` | dict round-trips, numpy volumes, discovery, wildcards, threading, error handling |
| `scripts/test-interop.sh` | a C++, a Python and a C# node in one realm, each verifying the others' payloads field by field |

Every network test runs in a realm unique to its process, so a demo on the same
machine — or a second CI job on the same box — cannot perturb it.

**Cross-platform.** `tests/docker/` holds two images: a Debian one that builds
Zyre from source (the path a machine without a system Zyre takes) and runs all
three languages, and a MinGW one that compiles every translation unit for
Windows. The second exists because Windows-only defects are otherwise invisible
until someone tries — it is what caught `interface` being a macro in
`<objbase.h>`, which broke the build on Windows and nowhere else.

`.gitlab-ci.yml` runs the Linux suite, the sanitizers, the Windows portability
check and the interop test on every push, plus MSVC and macOS jobs that activate
once runners with those tags exist.

**Sanitizers**, when changing the transport:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build-tsan -j && ./build-tsan/test_network
```

---

## Troubleshooting

**Two devices cannot see each other.**

1. Are they on the same Wi-Fi network or switch? Discovery is link-local by
   design and does not cross routers.
2. **Guest Wi-Fi and "client isolation" block devices from seeing each other.**
   This is the single most common cause. A normal network, or a cable, works.
3. Is UDP port 5670 open? A host firewall will block the beacon.
4. On a machine with several networks, name the one you mean:
   `cfg.iface = "eth0"` (C++), `iface="eth0"` (Python/C#) — otherwise discovery
   may pick the wrong one.
5. On a Meta Quest / Android device, see [Unity / Meta Quest](#unity--meta-quest):
   without the multicast permission the headset receives nothing.

Run `uninet-discover` on both machines; whichever one shows an empty list is the
one with the problem.

**Devices see each other but messages do not arrive.** Check the realms match,
and check the subject: `domain.D1` does not match a subscription to `domain.D1.>`
— use `domain.>` to catch everything below `domain`.

**`libuninet_c.so: cannot open shared object file`.** Add its directory to
`LD_LIBRARY_PATH`, or on Windows place `uninet_c.dll` next to the executable. In
Unity it belongs in `Assets/Plugins/<platform>/`.

**The Python extension imports but `join()` fails.** Check
`uninet.zyre_version()` — if that works the library is loaded correctly and the
problem is the network, not the build.

---

## Licence

MIT. See [`LICENSE`](LICENSE).

Depends on ZeroMQ/czmq/Zyre (MPL-2.0), zlib, and optionally liblz4.
