# UniNet

**Devices on a network find each other and talk. Nobody configures anything.**

```cpp
auto net = uninet::Session::join("Headset");
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
One codec, one wire format, three languages. See [Data](#data-json-in-cbor-on-the-wire-json-out).

---

## Contents

- [Why it exists](#why-it-exists)
- [Install](#install)
- [Quick start: C++](#quick-start-c) · [Python](#quick-start-python) · [C#](#quick-start-c-1)
- [Native library: where the .so / .dll / .dylib goes](#native-library-where-the-so--dll--dylib-goes)
- [Data: JSON in, CBOR on the wire, JSON out](#data-json-in-cbor-on-the-wire-json-out)
- [Large payloads: files, volumes, meshes](#large-payloads-files-volumes-meshes)
- [What each language can do](#what-each-language-can-do)
- [Finding devices](#finding-devices)
- [Realms: keeping setups apart](#realms-keeping-setups-apart)
- [Links without multicast (USB, VPN, routed)](#links-without-multicast-usb-tether-vpn-routed-networks)
- [Unity / Meta Quest](#unity--meta-quest)
- [3D Slicer](#3d-slicer)
- [Command-line tools](#command-line-tools)
- [Performance](#performance)
- [How it works](#how-it-works)
- [Who uses it](#who-uses-it)
- [Testing](#testing)
- [Troubleshooting](#troubleshooting)

---

## Why it exists

Most small distributed setups start the same way. A few programs, often in
different languages, need to exchange data on a local network. Each one ends up
with its own copy of the messaging code, and each one has a broker address
written into it somewhere. Moving to another room, a new laptop, or a fresh DHCP
lease then means editing several codebases and redeploying all of them.

UniNet removes the address and the broker at the same time. Programs announce
themselves on the local network and connect directly to each other. There is
nothing to install on a server, because there is no server.

**What UniNet gives you**

| | |
|---|---|
| **Zero configuration** | Devices discover each other. No addresses, anywhere. |
| **No broker** | Peer-to-peer. Nothing to install, start, or keep running. |
| **Three languages** | C++, Python, C#, one data model, identical bytes on the wire. |
| **Presence** | Know who is on the network, and when someone joins or leaves. |
| **JSON or CBOR** | Write JSON, send compact binary, read JSON. Your choice per call. |
| **Fast** | 18k messages/s at 60 KB each, same-host loopback (see [Performance](#performance)). |

---

## Install

### Prerequisites

UniNet needs **ZeroMQ/Zyre** (the peer-to-peer layer), **zlib**, and optionally
**liblz4**. Every one of them is fetched and compiled by the build when the
machine does not have it, so a compiler and CMake are the only real
prerequisites and on most machines you can skip straight to the build.
Installing them yourself only makes the first build faster.

| OS | install prerequisites |
|---|---|
| **Ubuntu/Debian** | `sudo apt install build-essential cmake pkg-config git zlib1g-dev liblz4-dev libzmq3-dev libczmq-dev` |
| **Fedora** | `sudo dnf install gcc-c++ cmake pkgconf-pkg-config git zlib-devel lz4-devel zeromq-devel czmq-devel` |
| **Arch** | `sudo pacman -S base-devel cmake pkgconf git zlib lz4 zeromq czmq zyre` |
| **macOS** | `brew install cmake pkg-config git zlib lz4 zeromq czmq zyre` |
| **Windows** | `scripts/bootstrap.ps1` (clones vcpkg for the dependencies); needs Git, CMake, and VS 2022's "Desktop development with C++" workload |

**Zyre itself is not packaged by Ubuntu, Debian or Fedora.** There is no
`libzyre-dev` to install there, so CMake fetches and builds Zyre for you on the
first configure. Nothing extra is required from you; `git` is in the list above
because that fetch needs it.

The `libzmq3-dev` and `libczmq-dev` entries are what Zyre is built *against*.
They are optional in the sense that CMake will fetch and build those too if they
are absent, but installing them turns a several-minute first build into about
thirty seconds. Arch and Homebrew package Zyre as well, so on those two nothing
is built from source at all.

What CMake reports tells you which path it took:

```
-- UniNet: using system zyre 2.0.1.               <- nothing built from source
-- UniNet: using system libzmq 4.3.5.             <- only zyre is built
-- UniNet: fetching libzmq.                       <- libzmq is built too
-- UniNet: zlib headers not found, building ...   <- even zlib is built
```

> **`zlib1g-dev` is the one people are surprised by.** Every Linux desktop has
> `libz` as a library and almost none has its headers, so `Could NOT find ZLIB`
> used to be the first thing a clean machine saw — for the compression tier that
> is meant to be the one that is always available. It is fetched now too.

To force a source build of everything, pass `-DUNINET_SELF_CONTAINED=ON`. Do
that for anything you are going to copy to another machine - see
[Native library](#native-library-where-the-so--dll--dylib-goes).

**One command instead of all of the above**, on Linux and macOS:

```bash
./scripts/bootstrap.sh            # prerequisites, build, tests
./scripts/bootstrap.sh --python   # ...and the Python extension
```

and `.\scripts\bootstrap.ps1` on Windows, which clones vcpkg for the
dependencies.

### Build

```bash
git clone https://github.com/JonasMht/UniNet.git && cd UniNet
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build -L uninet --no-tests=error --output-on-failure
```

No options needed: the C++ library, the C ABI and the Python extension are all
built by default. The Python extension is the one part with an outside
requirement, pybind11, which CMake locates through your interpreter. If it is
not installed, that one target is skipped with a note and everything else still
builds.

That produces:

| artifact | what it is |
|---|---|
| `build/libuninet.a` | the C++ library |
| `build/libuninet_c.so` · `.dylib` · `build\Release\uninet_c.dll` | the C ABI, for C# / Unity / any FFI. See [Native library](#native-library-where-the-so--dll--dylib-goes) |
| `python/uninet/_uninet*.so` | the Python extension, importable with `PYTHONPATH=python` |
| `build/uninet-discover` | CLI: what is on my network? |
| `build/uninet-demo` | the demo (see [Command-line tools](#command-line-tools)) |
| `build/uninet-file-transfer` | send a file, no address needed |
| `build/uninet-benchmark` | end-to-end network benchmark |
| `build/uninet-benchmark-codec` | codec-only benchmark |

On Windows with MSVC every one of those lives under `build\Release\` and the
executables end in `.exe`: MSVC is a multi-config generator.

**Python:**

```bash
pip install .
```

**CMake options** (all optional):

| option | default | meaning |
|---|---|---|
| `UNINET_BUILD_CABI` | ON | build `libuninet_c` for C#/FFI |
| `UNINET_BUILD_PYTHON` | ON | build the Python extension; skipped with a note if pybind11 is absent |
| `UNINET_LZ4` | ON | LZ4 compression tier, auto-detected |
| `UNINET_SYSTEM_ZYRE` | ON | use an installed zyre; OFF forces a source build |
| `UNINET_SELF_CONTAINED` | OFF | build **every** dependency from source, so the result can be copied to another machine |
| `UNINET_WERROR` | OFF | warnings are errors (what CI uses) |

---

## Quick start: C++

```cpp
#include "uninet/session.h"
#include "uninet/json.h"

int main() {
    // The whole setup. The name is what other devices will show for this one.
    auto net = uninet::Session::join("Recorder");

    // Who else is here: now, and as devices come and go.
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

    // Send, to everyone...
    uninet::Cbor data = uninet::Cbor::map();
    data.set("code", uninet::Cbor::text("update"));
    net->publish("domain.D1", data);

    // ...or from JSON, whichever is more convenient at the call site.
    net->publish_json("domain.D1", R"({"code":"update","n":42})");

    // ...or privately, to one device.
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

`add_subdirectory` is the supported route. There is deliberately no
`find_package(UniNet)`: the library links publicly against either a
pkg-config-imported Zyre or a Zyre built from source in your tree, and neither
survives a CMake export set, so a config file would work in one case and fail
confusingly in the other.

**Configuration**, when you need it: the defaults are right for a single
network:

```cpp
uninet::SessionConfig cfg;
cfg.role  = "server";          // free-form label shown to other devices
cfg.app   = "my-app";          // owning application
cfg.realm = "or-3";            // see Realms below
cfg.iface = "eth0";            // only on a machine with several networks
auto net = uninet::Session::join("Recorder", cfg);
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
    "nested": {"session": "run-04"},
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
    Console.WriteLine($"{p.Name}  {p.Endpoint}  {p.Role}");
```

**In Unity, one extra line is required.** Messages arrive on a background
network thread, and touching the Unity API from there crashes the player. By
default UniNet queues events for you; drain the queue from `Update()`:

```csharp
void Update() => net.Update();     // delivers callbacks on the main thread
```

Outside Unity (a console app or service) pass `marshalToCaller: false` to get
events immediately on the network thread instead. The example above, run in a
console app without either of those, receives nothing at all: the events are
sitting in a queue nobody drains.

> The queue is bounded at 100,000 pending events. A consumer that never calls
> `Update()` does not grow without limit; the oldest events are dropped.

**Run the demo**, which is that code with two sessions in one process:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
dotnet run --project csharp/UniNetDemo
```

The project copies the native library out of `build/` for you. Where that file
goes for your own application, and for Unity, is the next section.

---

## Native library: where the .so / .dll / .dylib goes

C# does not talk to the network itself. It calls a small C library compiled from
the C++ core - `libuninet_c.so`, `uninet_c.dll` or `libuninet_c.dylib` - and that
file has to exist somewhere the runtime looks. This is the one part of using
UniNet from C# that is not automatic, so here is all of it.

**What a build produces**, from `cmake -S . -B build && cmake --build build -j`:

| platform | file | where the build puts it |
|---|---|---|
| Linux | `libuninet_c.so` | `build/` |
| macOS | `libuninet_c.dylib` | `build/` |
| Windows (MSVC) | `uninet_c.dll` | `build\Release\` (MSVC is multi-config) |
| Android/Quest | `libuninet_c.so` | `build-android/`, via `./scripts/build-for-android.sh` |

`./scripts/build-native.sh` builds just that library, with every dependency
compiled in so it can be copied to another machine, and prints the table below
filled in for what it built.

**Where to put it:**

| consumer | where it goes |
|---|---|
| A .NET project referencing `csharp/UniNet/UniNet.csproj` | **nothing to do** - the project copies it next to your executable. Point it elsewhere with `-p:UniNetNativeDir=/path/to/it` |
| A published .NET app (`dotnet publish`) | next to the `.exe` / app `.dll` |
| Any other .NET app | the output directory (`bin/Debug/net8.0/`), or a directory on `LD_LIBRARY_PATH` (Linux), `DYLD_LIBRARY_PATH` (macOS), `PATH` (Windows) |
| **Unity, desktop** (Editor and standalone player) | `Assets/Plugins/x86_64/libuninet_c.so` · `Assets/Plugins/x86_64/uninet_c.dll` · `Assets/Plugins/x86_64/libuninet_c.dylib`. In the Inspector set the platform to Standalone and the CPU to x86_64 |
| **Unity, Quest / Android** | `Assets/Plugins/Android/libs/arm64-v8a/libuninet_c.so` |
| Python | not applicable - the Python package contains its own extension |

Three things about this that cost people an afternoon:

1. **The file name matters, exactly.** `[DllImport("uninet_c")]` makes .NET look
   for `libuninet_c.so` on Linux, `libuninet_c.dylib` on macOS and
   `uninet_c.dll` on Windows - note that it does **not** add the `lib` prefix on
   Windows. A MinGW build calls its output `libuninet_c.dll` by default, which
   nothing can then load; UniNet's CMake sets the prefix to empty on Windows so
   both MSVC and MinGW produce the right name. Unity matches file names the same
   way.
2. **The Unity Editor loads a native plugin once per session.** Replacing the
   file while the Editor is open changes nothing until you restart it. This is
   the "my rebuild did nothing" of Unity plugins.
3. **A library built here is not automatically portable there.** A normal build
   links whatever the build machine has - the ZeroMQ stack, and whatever else
   czmq found. Copy that file to a machine without them and it fails to load,
   naming a library the user never installed. `./scripts/build-native.sh`
   (or `-DUNINET_SELF_CONTAINED=ON`) compiles ZeroMQ, czmq, zyre, lz4 and zlib
   into the library so nothing but the C++ runtime has to travel with it, and
   it runs `ldd` afterwards to tell you if anything else still does. Verified by
   loading such a build on a machine with no ZeroMQ installed at all.

   The C++ runtime is the remaining constraint, and it is a one-way one: a
   library built against glibc 2.39 loads on 2.39 and newer, not on 2.35. If you
   hand out binaries, build them on the oldest system you support (a container is
   the easy way), not on the newest.

If it is still not found, the error says what to do: UniNet catches .NET's
`DllNotFoundException` and replaces the list of probe paths with the build
command and this table.

---

## Data: JSON in, CBOR on the wire, JSON out

UniNet sends **CBOR**, a compact binary format that carries typed values and
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
| float array | ordinary JSON array (on one line, even when pretty-printing, a 12288-element array on 12288 lines helps nobody) |
| integer > 2⁵³ | survives CBOR exactly; loses precision in JSON consumers |
| NaN / Infinity | `null` (JSON has no representation for them) |

---

## Large payloads: files, volumes, meshes

`publish()` sends a message: it arrives as one unit, and both ends hold it whole
in memory. That is the right tool up to a few megabytes.

A 200 MB volume or a large dataset file is not a message. `Blob` chunks it, streams
it, reassembles it on the far side, and reports progress at both ends:

```python
blob = uninet.Blob(net, "volumes")

# receiving
blob.on_progress(lambda info, done: print(f"{100*done/info.size:.0f}%"))
blob.on_received(lambda info, data: save(info.name, data))

# sending: metadata travels with the payload
blob.send("scan-volume", volume, meta={
    "dtype": str(volume.dtype),
    "shape": list(volume.shape),
    "spacing": [0.5, 0.5, 1.0],
})
blob.send_file("/path/to/dataset.zip")
```

```cpp
uninet::Blob blob(*net, "volumes");
blob.on_received([](const uninet::BlobInfo& info, const uninet::Bytes& data) {
    save(info.name, data);
});
blob.send_file("/path/to/dataset.zip");
```

```csharp
using var blob = new Blob(net, "volumes");
blob.Received += (info, data) => File.WriteAllBytes(info.Name, data);
blob.Progress += (info, done) => Console.WriteLine($"{100.0 * done / info.Size:F0}%");
blob.SendFile("/path/to/dataset.zip");
```

All three interoperate: a blob sent from Python arrives byte-identical in C# and
C++, with its metadata intact.

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

> Call `np.ascontiguousarray(a)` before sending a numpy array, a sliced or
> transposed array is not contiguous, and the buffer protocol needs it to be.

See [`examples/`](examples/) for complete, runnable versions of all of this.

---

## What each language can do

The three bindings are deliberately at parity. Anything in this table works the
same way, over the same wire bytes, in all three.

| | C++ | Python | C# |
|---|:-:|:-:|:-:|
| join / close | ● | ● | ● |
| publish (JSON or native) | ● | ● | ● |
| publish to one peer | ● | ● | ● |
| subscribe (JSON / native) | ● | ● | ● |
| subscribe to raw CBOR | ● | ● | ● |
| peers, presence events | ● | ● | ● |
| peer headers | ● | ● | ● |
| large transfers (`Blob`) | ● | ● | ● |
| transfer progress and failure | ● | ● | ● |
| custom advertised headers | ● | ● | ● |
| compression choice | ● | ● | ● |
| gossip / non-multicast links | ● | ● | ● |
| realm isolation | ● | ● | ● |
| tuning `Blob` (chunk size, limits) | ● | ● | |
| listing this machine's interfaces | ● | ● | |
| turning auto-reconnect off / tuning it | ● | ● | |

The last three rows are the honest edges of "at parity": C# gets UniNet's
defaults for them and cannot change them, because the C ABI it goes through does
not carry those settings yet. Everything above the line is genuinely the same in
all three.

`publish` tells you when a message could not be sent: C++ and Python return
false, C# throws. A transfer
that cannot start says so rather than returning a plausible-looking id.

**Names that are the same thing in each language**, since they are the ones
people mix up: `peer.host()` / `peer.host` / `Peer.Host` is the machine's
hostname, and `peer.endpoint()` / `peer.endpoint` / `Peer.Endpoint` is its IP
address without the port. (Before 0.2.1 the C# `Peer.Host` returned the address,
so the same name meant two different things - if you print an address from C#,
it is `Endpoint`.)

---

### One lifetime rule

A handler runs on the network thread and keeps running until the session is
closed, so anything it captures must outlive the session.

```cpp
std::vector<Msg> received;                 // declared BEFORE the session,
auto net = uninet::Session::join("Viewer"); // so it is destroyed AFTER it
net->subscribe("t.>", [&](const uninet::Envelope& e) { received.push_back(...); });
```

Declared the other way round, `received` dies first (C++ destroys locals in
reverse order) and a message arriving in that window writes to freed memory.
Calling `net->close()` before the captured state goes away is the explicit way
to be sure, and is what the tests do.

Python and C# are not exposed to this: the garbage collector keeps captured
objects alive as long as the handler can reach them.

---

## Finding devices

Every device advertises a name, and optionally a role, an app, and any headers
you choose. All of it arrives with the discovery beacon, so a peer list is
complete the moment a device appears, no follow-up query.

```cpp
for (const uninet::Peer& p : net->peers()) {
    p.uuid;       // address for a private message
    p.name;       // "Headset"
    p.address;    // "tcp://192.168.1.31:35001": observed, not self-reported
    p.role();     // "headset"
    p.app();      // "my-app"
    p.header("anything-you-set");
}
```

`on_peer_found` also replays the devices already present, so registration order
never changes what you see.

> **`address` is the address the connection actually came from**, not one the
> peer claims. A device's own idea of its address is wrong behind NAT and
> forgeable everywhere.

---

> **`uninet.*` is reserved** for the library's own traffic (large transfers use
> `uninet.blob.<subject>`). Subscribing to `uninet.>` or to `>` will deliver
> those internal frames to your handler.

---

## Realms: keeping setups apart

Devices only see devices in the **same realm**. It is the one setting that ever
needs changing, and it exists for two situations:

- Two independent setups sharing one physical network.
- A developer's laptop that must not join a live production session.

```cpp
cfg.realm = "or-3";                          // C++
```
```python
uninet.join("Tool", realm="or-3")            # Python
```
```csharp
Session.Join("Tool", realm: "or-3");         // C#
```

Realms isolate both messaging and the peer list, a device in another realm is
invisible, not merely unreachable.

---

## Links without multicast (USB tether, VPN, routed networks)

Discovery uses a UDP beacon, which needs the devices to share a broadcast
domain. Some links do not provide one:

- a device tethered over USB and reached through a port forward
- a VPN
- two subnets separated by a router
- a cloud host

For those, ZRE offers a second discovery mode. One node binds a rendezvous
endpoint, the others dial it, and no multicast is involved at all:

Throughout the examples below, `RENDEZVOUS_ADDR` stands for **the address of
the rendezvous machine as seen by the node doing the dialling**. There is no
fixed value: it depends entirely on the link.

| link | what `RENDEZVOUS_ADDR` is |
|---|---|
| same LAN | the machine's LAN address, e.g. `192.168.1.10` |
| VPN (WireGuard, Tailscale, ...) | its **VPN** address, not its LAN one, e.g. `10.0.0.10` |
| USB via `adb reverse` | always `127.0.0.1`, because the tunnel makes the far end look local |
| cloud host | its public address or DNS name |

`tcp://*:PORT` on the binding side means "every interface on this machine" and
never changes.

```cpp
// the rendezvous node: binds, so it uses * and needs no address of its own
uninet::SessionConfig host;
host.gossip_bind = "tcp://*:5670";
host.endpoint    = "tcp://*:5671";
auto a = uninet::Session::join("Recorder", host);

// every other node: dials, so it needs the rendezvous machine's address
uninet::SessionConfig peer;
peer.gossip_connect = "tcp://RENDEZVOUS_ADDR:5670";
auto b = uninet::Session::join("Laptop", peer);
```

```python
a = uninet.join("Recorder", gossip_bind="tcp://*:5670", endpoint="tcp://*:5671")
b = uninet.join("Laptop",   gossip_connect="tcp://RENDEZVOUS_ADDR:5670")
```

Run `uninet-discover` or `ip addr` on the rendezvous machine to find the address
the other side should use. If the two are on a VPN, use the VPN address: the LAN
one is usually not reachable across it.

Everything above this line works unchanged: same subjects, same payloads, same
presence events.

### USB-tethered Android device (Meta Quest and similar)

This works, with one thing to be careful about. `adb` gives you the tunnel:

```bash
# on the computer: let the headset reach the computer's rendezvous port
adb reverse tcp:5670 tcp:5670
```

The headset then dials `tcp://127.0.0.1:5670`. Over `adb reverse` the address is
**always** `127.0.0.1`, whatever the machines' real addresses are: the tunnel
makes the computer's port appear on the headset's own loopback. This is the one
case where the address is fixed and you can hardcode it.

**The part that needs attention:** gossip carries only the introductions. Once
two nodes know about each other they open direct TCP connections, so each node's
`endpoint` has to be an address the other can actually reach. Over a single
forwarded port that is not automatic, so forward the data port too and tell each
side what to advertise:

```bash
adb reverse tcp:5670 tcp:5670     # rendezvous
adb reverse tcp:5671 tcp:5671     # the computer's data endpoint
adb forward tcp:5672 tcp:5672     # the headset's data endpoint
```

```csharp
// on the headset
var net = Session.Join("Headset",
    gossipConnect: "tcp://127.0.0.1:5670",
    endpoint:      "tcp://*:5672",
    advertisedEndpoint: "tcp://127.0.0.1:5672");
```

`advertised_endpoint` exists precisely for this: it is what the node tells peers
to dial, when that differs from what it binds.

Whether this is worth it depends on your case. Over Wi-Fi the beacon needs no
setup at all, so USB is for when Wi-Fi is unavailable, blocked by client
isolation, or too slow. A USB 3 link is faster and far more predictable than
congested Wi-Fi, which can matter for large transfers.

> Android also filters multicast in the Wi-Fi driver, so a tethered device needs
> the multicast lock described below regardless of which discovery mode you use,
> unless you use gossip exclusively.

---

## Unity / Meta Quest

**Desktop first** (the Editor, and Windows/Linux/macOS players): build the
native library and copy it into `Assets/Plugins/x86_64/`.

```bash
./scripts/build-native.sh --into /path/to/UnityProject/Assets/Plugins/x86_64
```

In the Inspector set the platform to Standalone and the CPU to x86_64. The
Editor loads a native plugin once per session, so restart it after replacing the
file. Full table: [Native library](#native-library-where-the-so--dll--dylib-goes).

**Then Android**, which is a different library for a different CPU - a Quest
build needs both if you also run in the Editor.

**1. Build the native library for Android ARM64:**

```bash
./scripts/build-for-android.sh                      # finds Unity's own NDK
./scripts/build-for-android.sh /path/to/android-ndk # or say where it is
```

The no-argument form looks in `~/Unity/Hub/Editor/*/Editor/Data/PlaybackEngines/AndroidPlayer/NDK`,
which is where the Linux Unity Hub puts it. Anywhere else - macOS, Windows, a
standalone NDK - pass the path; the script says so if it finds nothing.

It cross-compiles zlib, libzmq, czmq, zyre and UniNet for `arm64-v8a` (API 24)
and prints where to copy the result:

```
Assets/Plugins/Android/libs/arm64-v8a/libuninet_c.so
```

Everything is linked statically except Android's own libraries, so that one file
is all that ships: `libz`, `liblog`, `libm`, `libdl`, `libc` and nothing else. In
the Unity inspector leave the platform set to Android / ARM64, which the
`arm64-v8a` directory name already implies.

**2. Add the C# sources.** Copy the whole of `csharp/UniNet/` into
`Assets/Plugins/UniNet/` - the `.cs` files and the `Unity/` subdirectory with
the Android multicast lock in it. They compile unchanged under both scripting
backends and both API compatibility levels; nothing needs to be edited for Unity.

If you would rather reference a built assembly than the sources, build it with

```bash
dotnet build csharp/UniNet -c Release      # bin/Release/netstandard2.1/UniNet.dll
```

and drop that DLL in `Assets/Plugins/`. The sources are the simpler route in a
Unity project, and the only one that gives you the multicast lock.

> **IL2CPP is supported, and that is not automatic.** Android and iOS players are
> always IL2CPP, which is ahead-of-time: it has no JIT, so a method that native
> code calls back into has to exist as a real function at compile time. That means
> a **static** method carrying `[MonoPInvokeCallback]`, never a lambda or a closure.
> UniNet's callbacks are written that way, and the per-subscription state they need
> reaches them through a `GCHandle` passed in the C ABI's `user` pointer.
>
> This is worth knowing because the failure mode is nasty: a binding that uses
> lambdas compiles cleanly, runs perfectly in the editor, and then throws
> `NotSupportedException: To marshal a managed method, please add an attribute
> named 'MonoPInvokeCallback'` the first time a message arrives on the device.
> If you fork `Native.cs` or `Session.cs`, keep the callbacks static.

**3. Add the Wi-Fi multicast permission. This is not optional.**

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

It is a normal install-time permission, no runtime request needed.

**4. Acquire the lock before joining, release it on teardown.** A ready-made
helper lives at `csharp/UniNet/Unity/UniNetMulticastLock.cs`, which is inside the
directory you copy anyway (step 2), so there is nothing extra to fetch.

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
        _net = Session.Join("Headset", role: "headset", app: "my-app");

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
> Wi-Fi only. On a non-Wi-Fi interface the lock is harmless but does nothing -
> discovery works because those interfaces do not filter multicast.

---

## 3D Slicer

Slicer ships its own Python, so UniNet's extension has to be built against
**that** interpreter. One file does all of it — `scripts/UniNetSlicer.py`. It
finds Slicer, works out what that Slicer's Python needs, installs UniNet into
it, and can check again every time Slicer starts. Nothing to configure, no
cmake, and you do not have to know where Slicer is.

**In Slicer's Python console** (`View → Python Console`), which needs no
terminal at all:

```python
exec(open("/path/to/UniNet/scripts/UniNetSlicer.py").read())
```

```
[uninet] UniNet is not installed for this Slicer; installing it now
[uninet] uninet 0.2.0 is ready in this Slicer
[uninet] for a check at every start, run:  install_startup_hook()
```

**Or from a terminal**, with any Python (it drives Slicer's for you):

```bash
python3 scripts/UniNetSlicer.py              # find Slicer, install, verify
python3 scripts/UniNetSlicer.py status       # what is installed, and where
python3 scripts/UniNetSlicer.py hook         # check at every Slicer start
```

**Or with no checkout at all**, pasted into Slicer's Python console
(`View → Python Console`):

```python
import urllib.request as _u, json as _j, base64 as _b64, time as _t
_src = None
for _try in range(3):
    try:
        _src = _u.urlopen("https://raw.githubusercontent.com/JonasMht/UniNet/main/scripts/UniNetSlicer.py",
                          timeout=30).read().decode()
        break
    except Exception:
        if _try < 2:
            _t.sleep(1.5 **_try)          # transient failures are retried
if not _src:                              # raw CDN unreachable -> GitHub API
    _meta = _j.load(_u.urlopen("https://api.github.com/repos/JonasMht/UniNet/contents/scripts/UniNetSlicer.py",
                               timeout=30))
    _src = _b64.b64decode(_meta["content"]).decode()
exec(_src)
```

This downloads `scripts/UniNetSlicer.py` and runs it. The retries and the API
fallback are there on purpose: `raw.githubusercontent.com` occasionally answers
`HTTP 429 Too Many Requests` when its CDN is under load. That is **transient
throttling, not a broken link** — the file is still at that address, and the
same fetch succeeds a moment later, so do not "fix" the URL (a wrong branch or
path would return `HTTP 404`, not 429). If even the GitHub API route fails,
this machine cannot reach github.com at all. Behind a proxy or on an offline
machine, copy `scripts/UniNetSlicer.py` across (it is one dependency-free file)
and use the first form; `UNINET_GIT_URL` points the source-build fallback at an
internal mirror if you keep one.

Afterwards `import uninet` works in any Slicer module. The first install builds
from source and takes a few minutes; every one after that is a second, because
the build leaves a wheel in the cache.

### Giving it to somebody else

```bash
python3 scripts/UniNetSlicer.py wheel
#   ~/.cache/uninet/wheels/uninet-0.2.0-cp39-cp39-linux_x86_64.whl
```

Put that file next to `UniNetSlicer.py` (or point `UNINET_WHEEL` at it) and
anyone with the same Slicer version installs in seconds **with no compiler, no
checkout and no network**. ZeroMQ, czmq and zyre are compiled into it — those
are the ones nobody has — so the other machine needs nothing but its own C++
runtime.

> Everything else the build happens to find is still linked normally, and a
> library in `/usr/local` or a home directory is one *your* machine has and the
> next one does not. `wheel` runs `ldd` over what it built and says so when that
> happens; the answer is to build the wheel on a plain machine or in a
> container. A wheel built here on a developer box needed `liblz4.so.1` from
> `/usr/local/lib`, which is exactly the case this warns about.

### The startup check

`hook` adds a small managed block to Slicer's `.slicerrc.py`:

```
python3 scripts/UniNetSlicer.py hook       # add it
python3 scripts/UniNetSlicer.py unhook     # remove it, leaving your own rc alone
```

At every start it checks that UniNet is importable, which costs a few
milliseconds when it is. When it is not — a fresh machine, or a Slicer that was
just updated to a new version with an empty `site-packages` — it asks whether to
install it, shows the build as it happens, and never blocks startup on its own.
The dialog's three buttons are **Yes**, **No** (ask again next time) and
**Ignore** (never ask again for this Slicer). To undo that last one, from
Slicer's Python console:

```python
slicer.app.settings().setValue("UniNet/skipStartupInstall", "false")
```

The block is written where Slicer will actually read it: Slicer prefers
`.slicerrc.py` in its own directory over the one in your home directory, and a
hook written to the wrong one never runs and never says so.

### Undoing it, and other Slicers

```bash
python3 scripts/UniNetSlicer.py uninstall            # remove UniNet from Slicer
python3 scripts/UniNetSlicer.py unhook               # stop checking at startup
```

With more than one Slicer installed, every command takes `--slicer`, and
`status` says which one it picked:

```bash
python3 scripts/UniNetSlicer.py status --slicer ~/Documents/Slicer-5.6.2-linux-amd64
python3 scripts/UniNetSlicer.py install --slicer ~/Documents/Slicer-5.8.1-linux-amd64
```

Without it, the newest installation found is used and the others are listed, so
you can see that a choice was made. `UNINET_SLICER` does the same thing as
`--slicer` for every command at once.

### From your own module

If you ship a Slicer module that needs UniNet, let it install itself rather than
putting instructions in a README:

```python
import UniNetSlicer                      # add scripts/ to sys.path, or vendor the file
uninet = UniNetSlicer.ensure()           # a few ms when it is already there
```

<details>
<summary>What it is doing, and why any of it is necessary</summary>

A Slicer binary release has four traps. Every one of them produces an error that
names something other than the actual problem:

1. **It ships no Python headers.** `lib/Python/include/python3.9` contains only
   `pyconfig.h`, so any build fails on `Python.h: No such file or directory`.
   The installer fetches the matching CPython source headers from python.org and
   combines them with Slicer's own `pyconfig.h`. Slicer's Python is stock
   CPython, so the upstream headers match exactly.
2. **Slicer's Python remembers the compiler it was built with.** On the Linux
   releases that is `/opt/rh/devtoolset-7/root/usr/bin/gcc`, exported as `CC`
   during the build, and cmake stops with *"Could not find compiler set in
   environment variable CC"* naming a path that exists on nobody's machine.
3. **pybind11 reads a different variable than scikit-build-core sets.** The
   include directory has to arrive as `Python_INCLUDE_DIR`, `Python3_INCLUDE_DIR`
   *and* as a `-I` flag, because different Slicer releases ship different
   pybind11 versions that read different ones.
4. **`python-real` cannot run standalone**, and the environment inside a running
   Slicer is not the environment a compiler should inherit — `LD_LIBRARY_PATH`
   points into Slicer's own libraries. The installer builds in the environment
   Slicer was *started* from.

It also always builds ZeroMQ, czmq and zyre from source rather than linking the
system ones. That costs a few minutes once. Linked against a system `libzyre`,
the wheel imports only on machines that also have it, and the error lands on a
colleague, at import time, reading `libzyre.so.2: cannot open shared object
file` about a library they never installed.

Where things go: the wheel, the fetched headers and the build tree live in
`~/.cache/uninet` (`%LOCALAPPDATA%\UniNet\cache` on Windows), never inside
Slicer — a Slicer update replaces that directory wholesale. If Slicer's
`site-packages` is not writable (a system-wide install under `/opt` or
`Program Files`), UniNet is installed into that cache instead, and the startup
hook is what puts it on Slicer's path.

Environment overrides, all optional: `UNINET_SLICER`, `UNINET_SOURCE`,
`UNINET_WHEEL`, `UNINET_CACHE`, `UNINET_CC`/`UNINET_CXX`.

Tested against Slicer 5.8.1 (Python 3.9.10) on Linux, including on a machine
with no cmake, no zlib headers and no ZeroMQ (`scripts/test-slicer-setup.sh`,
and `python/tests/test_slicer_setup.py` for the decisions it makes).
`scripts/build-for-slicer.sh` still works and now forwards here.
</details>

> **If a rebuild seems to change nothing**, an older copy in Slicer's
> `site-packages` is shadowing yours. An install made by copying files there by
> hand has no metadata for pip to find, so the installer removes such a copy
> before installing over it.

### The shape a Slicer module wants

```python
import collections, qt, uninet

class MyModuleLogic:
    def __init__(self):
        # Filled on UniNet's network thread, drained on Slicer's main thread.
        self._inbox = collections.deque()
        self._pump = qt.QTimer()                 # created on the main thread
        self._pump.setInterval(16)               # ~60 Hz, same idea as Unity's Update()
        self._pump.timeout.connect(self._drain)
        self._pump.start()

        self.net = uninet.join("Slicer Viewer", role="viewer", app="my-app")
        self.net.subscribe("app.v1.>", self.on_message)
        self.net.on_peer_found(self.on_peer)

    def on_message(self, msg):
        # Network thread. Do no Slicer work here: just hand it over.
        self._inbox.append(msg.data)

    def _drain(self):
        while self._inbox:
            data = self._inbox.popleft()         # main thread: safe from here on
            if data.get("code") == "update":
                ...

    def on_peer(self, peer):
        print(f"UniNet: {peer.name} ({peer.role}) at {peer.endpoint}")

    def cleanup(self):
        self._pump.stop()
        self.net.close()
```

> **The threading rule is the same as Unity's**: callbacks arrive on a
> background thread. Marshal to the main thread before touching VTK, Qt or the
> MRML scene.
>
> **`qt.QTimer.singleShot(0, fn)` does not work for this**, even though it is
> the usual Slicer idiom. Qt refuses to start a timer on a thread it did not
> create, and UniNet's delivery thread is a plain pthread, so Slicer logs
>
> ```
> QObject::startTimer: Timers can only be used with threads started with QThread
> ```
>
> and your handler never runs. The message goes to the terminal, not to the
> Python console, so it usually is not seen at all: the symptom is simply that
> nothing arrives. A queue plus a timer created on the main thread, as above, is
> the shape that works. It also preserves message order.

Volumes are the realistic payload, and numpy is already bundled with Slicer:

```python
blob = uninet.Blob(self.net, "volumes")
blob.on_received(self.on_volume)

def on_volume(self, info, data):
    import numpy as np
    volume = np.frombuffer(data, dtype=np.dtype(info.meta["dtype"]))
    volume = volume.reshape(info.meta["shape"])
```

A device list widget, since Slicer modules usually want one:

```python
def refresh_device_list(self):
    self.deviceCombo.clear()
    for p in self.net.peers():
        self.deviceCombo.addItem(f"{p.name} - {p.role} ({p.endpoint})", p.uuid)
```

---

## Command-line tools

The build leaves these in `build/` (`build\Release\` on Windows). The commands
below write them without a path, which works after

```bash
cmake --install build --prefix ~/.local     # or any prefix on your PATH
```

Otherwise put `./build/` in front: `./build/uninet-discover --once`.

### `uninet-discover`, what is on my network?

The tool to run when someone says "the headset can't see the server".

```bash
uninet-discover              # live view: devices arriving and leaving
uninet-discover --once       # one snapshot, then exit
```

```
DEVICE                     ADDRESS          ROLE         APP
Recorder                   192.168.1.10     server       my-app
Headset                    192.168.1.31     headset      my-app
Laptop                     192.168.1.24     viewer       my-app

3 devices.
```

When it finds nothing, it says what to check, in order, in plain language.

| flag | meaning |
|---|---|
| `--once` | one snapshot instead of a live view |
| `--timeout <s>` | how long `--once` listens (default 3) |
| `--realm <name>` | only show devices in this realm |
| `--interface <n>` | which network to look on (`eth0`, or an IP) |
| `--interfaces` | list this machine's networks and what UniNet would pick |
| `--version` | the ZeroMQ/Zyre versions in use |

### `uninet-demo`: two devices talking, with nothing configured

```bash
uninet-demo "Laptop"
uninet-demo "Headset" --role headset      # another terminal, or another machine
```

Each prints the other arriving, then they exchange messages. Nobody types an
address. `scripts/demo.sh` runs three at once.

### `uninet-file-transfer`: send a file, no address needed

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
mesh payloads over the full stack: encode → compress → frame → TCP → unframe →
decompress → decode → dispatch:

| payload | size on the wire | messages/s | throughput | delivered |
|---|---|---|---|---|
| 512 verts | 7.7 KB | **38,115** | 280 MB/s | 300/300 |
| **4096 verts** (the live MR mesh) | 61 KB | **18,503** | 1,085 MB/s | 300/300 |
| 16384 verts | 246 KB | **9,777** | 2,292 MB/s | 300/300 |

**Cold-start discovery: 2 ms** for two processes on one machine.

> **These are same-host loopback figures.** Both sessions run on one machine, so
> what is measured is the protocol's own cost, not a network. A 1 GbE link caps
> at roughly 118 MB/s and Wi-Fi well below that, so the MB/s column is a ceiling
> on the software, not a rate you will see between two machines.
>
> **Expect discovery to take about a second on a real network**, not two
> milliseconds. ZRE's beacon interval and the Wi-Fi association dominate. Treat
> ~1 s as the number to design around and the 2 ms as a floor.

Codec-level numbers (encode/compress/frame in isolation) come from the separate
`uninet-benchmark-codec` target and are logged to `uninet_bench_log.csv`.

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
                  diagnostics.h (diagnostics(), enable_crash_log())
src/              one .cpp per header
python/           bindings.cpp (pybind11) · uninet/ (package)
                  tests/: test_uninet.py · test_slicer_setup.py
csharp/           UniNet/ (Session.cs · Native.cs · Blob.cs ·
                            Unity/UniNetMulticastLock.cs, Unity-only)
                  UniNetDemo/ (dotnet run --project csharp/UniNetDemo)
examples/         demo.cpp · file_transfer.cpp · python/ · android/ (Quest demo)
                  see examples/README.md
tests/            test_roundtrip (codec) · test_network · test_cabi (C)
                  test_diagnostics · test_reconnect (Linux only)
                  benchmark_codec.cpp (codec) · benchmark_network.cpp (end-to-end)
                  interop/: the three-language interop participants
                  docker/ : Linux and Windows-cross test images
tools/            uninet_discover.cpp · uninet_demo.cpp · uninet_file_transfer.cpp
scripts/          test-all.sh · test-interop.sh · demo.sh · bootstrap.{sh,ps1}
                  build-native.sh: the C ABI for C#/Unity, and where to put it
                  UniNetSlicer.py: install UniNet into 3D Slicer, from anywhere
                  build-for-android.sh · build-for-slicer.sh (-> UniNetSlicer.py)
                  test-on-android.sh · test-on-emulator.sh · check-il2cpp.sh
                  test-slicer-setup.sh · test-reconnect.sh (driven by ctest)
docs/             PROTOCOL.md
```

**Discovery** is ZRE's UDP beacon: each node broadcasts its presence on the
local link, peers hear it and open a direct TCP connection. Beacons carry a hop
limit of 1, so they never cross a router: discovery cannot leak into the rest of
the wider network.

**Delivery** is peer-to-peer TCP. A broadcast is a `SHOUT` to the realm group; an
addressed message is a `WHISPER` to one peer, which is a genuine unicast rather
than a broadcast everyone else filters. Echo suppression is free: ZRE never
delivers a node its own broadcast.

**The payload** is UniNet's own CBOR envelope, carrying the subject, the sender's
uuid, and your data, with routing in a clear header before the compressed body
so a receiver can filter without decompressing. See
[`docs/PROTOCOL.md`](docs/PROTOCOL.md).

**Licence note:** Zyre and czmq are MPL-2.0, libzmq is MPL-2.0. These are
file-level copyleft: you can link them into a proprietary application and ship
it; only modifications to *their* source files must be published. The other two
dependencies are permissive: zlib is under the zlib licence and LZ4 is BSD-2.
All four end up inside a self-contained build, so all four are worth naming in
whatever attribution your application ships. UniNet itself is MIT.

---

## Who uses it

Three applications, and what each one uses. The table is here because "we
migrated to UniNet" and "we use UniNet" are different claims, and the gap
between them is where the surprises live.

| | ThermoNav Server (C++) | ThermoNav Slicer (Python) | ThermoNav MR (C#/Quest) |
|---|---|---|---|
| join / publish / subscribe | yes | yes | yes |
| presence callbacks | yes | yes | yes |
| peer list | yes | yes | yes |
| `describe()` | yes | yes | yes |
| `last_error()` | yes | yes | yes |
| `diagnostics()` | yes | yes | yes |
| crash log | yes | yes | yes |
| network list | yes | yes | yes |
| dropped-event warning | n/a | yes | yes |
| `Blob` for large payloads | no | no | no |

The one real gap is `Blob`. Volumes and meshes still travel through
`publish()` as one large message, which works and is what the applications
did before. Moving them to `Blob` would give progress reporting and chunking,
but it changes the bytes on the wire between two applications at once, so it
is a coordinated change rather than a local one.

---

## Testing

```bash
./scripts/test-all.sh              # everything runnable natively
./scripts/test-all.sh --docker     # plus cross-platform and cross-language
```

Or run a suite directly:

```bash
ctest --test-dir build -L uninet --no-tests=error --output-on-failure
#   five suites: roundtrip (codec) · network · diagnostics · cabi ·
#   reconnect (Linux only: it needs a network namespace)
PYTHONPATH=python pytest python/tests -v       # Python, and the Slicer installer
./scripts/test-interop.sh                      # C++ <-> Python <-> C#
./scripts/test-slicer-setup.sh                 # the installer against a real Slicer
```

| suite | what it covers |
|---|---|
| `test_roundtrip` | CBOR round-trips, compression, framing, hostile frames |
| `test_network` | real discovery, departure, broadcast, unicast, realm isolation, concurrent publish/subscribe, large-payload transfer |
| `test_cabi` | the C ABI compiled **as C**: the exact path C#/Unity takes, including UTF-8, null-safety and pointer lifetimes |
| `python/tests` | dict round-trips, numpy volumes, discovery, wildcards, threading, error handling |
| `python/tests/test_slicer_setup.py` | every decision the Slicer installer makes - which Slicer, which interpreter, which wheel, what goes into `.slicerrc.py`, what the build environment ends up being - against a directory shaped like a Slicer install. Needs no Slicer and no network; the handful of cases that ask which compiler would be used skip themselves on a machine without one |
| `scripts/test-slicer-setup.sh` | the same installer against a **real Slicer**: install from nothing, import, two Slicer interpreters exchanging a message, the cached-wheel reinstall, the startup hook surviving a real Slicer start, and that the extension needs no system ZeroMQ |
| `scripts/test-interop.sh` | a C++, a Python and a C# node in one realm, each verifying the others' payloads field by field |
| `scripts/test-on-android.sh` | the codec, the C ABI and discovery running **on a connected Android device**, plus two nodes finding each other across the USB cable with no network |
| `scripts/test-on-emulator.sh` | the same suite on an **emulator**, so it runs with no hardware attached. Downloads the emulator and a system image once (~6 GB) into a scratch directory, boots headless, tests, shuts down. It is real Android userspace, but x86_64 and with emulated Wi-Fi, so it cannot answer questions about the multicast filtering that needs a `MulticastLock`: use a physical device for those |
| `scripts/check-il2cpp.sh` | compiles the C# binding with Unity's AOT class library and runs the real IL2CPP compiler over it, asserting every callback converts. Catches the Unity-only failure described under [Unity / Meta Quest](#unity--meta-quest), which no ordinary build or test run can see |

Every network test runs in a realm unique to its process, so a demo on the same
machine, or a second CI job on the same box: cannot perturb it.

**Cross-platform.** `tests/docker/` holds three images:

| image | what it verifies |
|---|---|
| `Dockerfile.linux` | Debian with Zyre built from source: the path a machine without a system Zyre takes: running C++, Python and C# |
| `Dockerfile.windows-check` | every translation unit compiles for `x86_64-w64-mingw32` at `-Werror` |
| `Dockerfile.windows-run` | the cross-compiled `.exe` files actually **run**, under Wine |

**How far the Windows coverage goes, precisely.** The MinGW images catch what is
otherwise invisible until someone tries a Windows build. They are what caught
`interface` being a macro in `<objbase.h>`, and `gmtime_r` not existing on MSVC.
Under Wine, `test_roundtrip.exe` passes in full (codec, compression, framing,
hostile input) and `test_cabi.exe` passes its JSON/CBOR and null-safety
sections as genuine Windows code.

What Wine **cannot** cover is discovery: czmq enumerates interfaces with
`GetAdaptersAddresses` and asserts on `ERROR_BUFFER_OVERFLOW`, a buffer-sizing
protocol Wine does not implement. The runner reports that as an *expected stop*,
never as a pass. Closing it needs a real Windows machine, and MSVC cannot run on
Linux at all.

**Two pipelines, and they cover different things.**

| | what runs |
|---|---|
| `.github/workflows/ci.yml` (GitHub Actions) | Linux (`-DUNINET_WERROR=ON`, ctest, pytest), **macOS 14 on Apple silicon**, and **Windows with real MSVC** - the only place the MSVC compile, link and C ABI test actually happen |
| `.gitlab-ci.yml` (the ICube forge) | the Linux suite, the sanitizers, both Windows-cross images and the interop test on every push, plus MSVC and macOS jobs that activate once runners with those tags exist |

**Sanitizers**, when changing the transport:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
      -DUNINET_SYSTEM_ZYRE=OFF     # zyre built with the sanitizer too, or its
cmake --build build-tsan -j        # threads are invisible and races are missed
./build-tsan/test_network
```

`./scripts/test-all.sh --sanitizers` does the same for ThreadSanitizer and
Address/UB together.

---

## Troubleshooting

**Start here: ask the library what it is doing.**

```bash
uninet-discover --interfaces      # from a terminal
```

```cpp
#include "uninet/diagnostics.h"
printf("%s", uninet::diagnostics().c_str());   // C++
```
```python
print(uninet.diagnostics())                    # Python
```
```csharp
Debug.Log(Session.Diagnostics());              // C#
```

It prints the library versions, which compression tiers this build has, every
network on the machine with the one discovery chose, and every live session with
its identity, peer count and reconnect count. Most of the questions below are
answered by that output directly, and it is the right thing to attach to a bug
report.

**When it crashes somewhere with no terminal** - a Quest player, a Slicer
module - ask for a crash report:

```cpp
#include "uninet/diagnostics.h"
uninet::enable_crash_log("/sdcard/Android/data/com.you.app/files/uninet-crash.log");
```

On a fatal signal it appends the same state plus the signal, the faulting
address and a backtrace. It is off unless you call it: a library has no business
taking over an application's fatal signal handling uninvited, and any handler
already installed is chained to, so a host's own crash reporting keeps working.


**Two devices cannot see each other.**

1. Are they on the same Wi-Fi network or switch? Discovery is link-local by
   design and does not cross routers.
2. **Guest Wi-Fi and "client isolation" block devices from seeing each other.**
   This is the single most common cause. A normal network, or a cable, works.
3. Is UDP port 5670 open? A host firewall will block the beacon.
4. On a machine with several networks, name the one you mean:
   `cfg.iface = "eth0"` (C++), `interface="eth0"` (Python), `iface: "eth0"`
   (C#, where `interface` is a keyword): otherwise discovery
   may pick the wrong one.
5. On a Meta Quest / Android device, see [Unity / Meta Quest](#unity--meta-quest):
   without the multicast permission the headset receives nothing.

Run `uninet-discover` on both machines; whichever one shows an empty list is the
one with the problem.

**A device stops being seen after the network changed.** It should not any
more: UniNet watches the machine's networks and rebuilds itself when the one in
use goes away or a better one appears. Wi-Fi dropping, moving between access
points, a cable pulled, a phone tethered, a laptop waking from sleep. Peers see
the device leave and rejoin, which is what actually happened.

It is on by default and costs an interface enumeration every two seconds. To
turn it off, or to react faster:

```cpp
uninet::SessionConfig cfg;
cfg.auto_reconnect     = false;   // something else owns reconnection
cfg.reconnect_poll_ms  = 500;     // notice sooner
```

`transport().reconnect_count()` reports how many times it has happened, which is
worth putting in a status line: a number that climbs steadily means the machine
is flapping between networks, not that UniNet is misbehaving.

**This machine has several networks and discovery is using the wrong one.**
This is the most common cause of "it just does not find anything" on a laptop,
and nothing on screen points at it. Discovery binds **one** interface. A machine
with a wired connection, Wi-Fi, a VPN and a couple of container bridges has
five or six, and the default is not necessarily the one you are thinking of.
Ask:

```bash
uninet-discover --interfaces
```

```
Networks on this machine:
  enp0s31f6        130.79.73.178     broadcast 130.79.73.255
  wlp0s20f3        192.168.1.24      broadcast 192.168.1.255
  docker0          172.18.0.1        broadcast 172.18.255.255
  ...
```

If the other device is on the Wi-Fi and discovery is on the wire, name the one
you mean: `--interface wlp0s20f3` for the tool, `cfg.iface` (C++),
`interface=` (Python), `iface:` (C#, where `interface` is a keyword). Docker and VPN interfaces are worth ruling out first: a beacon sent
on a bridge that routes nowhere reaches nobody, and every other symptom looks
healthy.

**Phone tethering and hotspots.** A phone sharing its connection presents
*separate* networks, not one:

| how the phone shares | typical range | who is on it |
|---|---|---|
| Wi-Fi hotspot | `192.168.43.x` | everything joined over Wi-Fi |
| USB tethering | `192.168.42.x` | only the machine holding the cable |

The phone routes between them so both reach the internet, but the discovery
beacon is link-local and **does not cross between them**. A machine on the
hotspot therefore cannot discover a machine on the USB tether of the same
phone, and no amount of interface pinning changes that. Two ways round it:

1. Put both on the same side, usually by joining both machines to the hotspot
   over Wi-Fi. Discovery then works normally.
2. Keep them where they are and skip the beacon: one side binds a rendezvous
   endpoint on an address the other can route to, and the other dials it. That
   is `gossip_bind` / `gossip_connect`, described under
   [Links without multicast](#links-without-multicast-usb-tether-vpn-routed-networks).

A USB Wi-Fi adapter acting as a hotspot is the ordinary case, not the awkward
one: every device that joins it, including the machine hosting it, is on a
single network and discovers everything else on it. Pin the interface to the
adapter if that machine also has another connection.

**Devices see each other but messages do not arrive.** Check the realms match,
and check the subject: `domain.D1` does not match a subscription to `domain.D1.>`
- use `domain.>` to catch everything below `domain`.

If messages flow one way only, the two builds disagree about compression. The
tier is chosen by the sender and travels on the wire, so a build without liblz4
cannot read anything from a build that has it, while its own messages, sent with
zlib, arrive perfectly. The result looks like a half-broken network: discovery
works, presence works, one direction works.

The receiving side prints

```
uninet: dropping messages from <uuid>: they are compressed with lz4 and this
build cannot decode that tier.
```

CMake now builds liblz4 from source when the system has none, so both sides
match by default and this should not arise. It can still be forced with
`-DUNINET_LZ4=OFF`, which is worth avoiding for anything that talks to a peer
built normally. `uninet.HAS_LZ4` (Python), `Session.HasLz4` (C#) and
`uninet_has_lz4()` (C) each report what a given build supports.

**`libuninet_c.so: cannot open shared object file`.** Add its directory to
`LD_LIBRARY_PATH`, or on Windows place `uninet_c.dll` next to the executable. In
Unity it belongs in `Assets/Plugins/x86_64/` (desktop) or
`Assets/Plugins/Android/libs/arm64-v8a/` (Quest). The full table, including what
each platform's file is called and why the name matters, is under
[Native library](#native-library-where-the-so--dll--dylib-goes). A .NET project
that references `csharp/UniNet/UniNet.csproj` gets the file copied for it.

**The Python extension imports but `join()` fails.** Check
`uninet.zyre_version()`, if that works the library is loaded correctly and the
problem is the network, not the build.

---

## Licence

MIT. See [`LICENSE`](LICENSE).

Depends on ZeroMQ/czmq/Zyre (MPL-2.0), zlib (zlib licence) and, optionally,
LZ4 (BSD-2-Clause). A `-DUNINET_SELF_CONTAINED=ON` build links all of them in,
so an application shipping that binary is distributing them too.
