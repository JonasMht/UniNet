# UniNet examples

Every example runs on **two terminals on one machine, or two machines on the
same network**: the commands are identical either way, because nothing is
configured. Start them in any order.

**Every command on this page is run from the repository root**, so paths like
`build/uninet-demo` and `examples/python/basic.py` work as written.

Build the C++ ones first:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

For the Python ones: `pip install .`, or `export PYTHONPATH=$PWD/python` after
that build - the extension is built by default whenever pybind11 is present.
The mesh and volume examples also need numpy (`pip install numpy`); UniNet
itself does not.

---

## Start here: two nodes talking

| | |
|---|---|
| **C++** | [`demo.cpp`](demo.cpp) → `build/uninet-demo "Laptop"` |
| **Python** | [`python/basic.py`](python/basic.py) → `python3 examples/python/basic.py alice` |

```bash
# terminal 1                      # terminal 2
build/uninet-demo "Laptop"        build/uninet-demo "Headset" --role headset
```

Each prints the other arriving, then they exchange messages. Nobody types an
address. `./scripts/demo.sh` runs three at once.

The **C# demo** ([`../csharp/UniNetDemo`](../csharp/UniNetDemo)) is one process
rather than two: it joins twice, in a realm private to that run, exchanges
messages between the two and exits. One terminal is all it needs:

```bash
dotnet run --project csharp/UniNetDemo
```

It needs the native library, which the project copies out of `build/` for you -
so build first. If you moved the build elsewhere, say where:
`dotnet run --project csharp/UniNetDemo -p:UniNetNativeDir=/path/to/it`.

---

## Sending data

| what | example | why this way |
|---|---|---|
| **Messages** (up to a few MB) | [`python/basic.py`](python/basic.py) | `publish()`, one call, arrives as one unit |
| **A mesh**, streaming | [`python/mesh_transfer.py`](python/mesh_transfer.py) | `publish()` at 20 Hz; float arrays take the binary fast path |
| **A file** | [`file_transfer.cpp`](file_transfer.cpp) · [`python/file_transfer.py`](python/file_transfer.py) | `Blob`: chunked, with progress |
| **A 3D volume** (numpy) | [`python/volume_transfer.py`](python/volume_transfer.py) | `Blob`: 64 MB+ with shape and dtype attached |

**The rule of thumb:** `publish()` for anything up to a few MB that you want as
one message. `Blob` for anything larger, or anything you want a progress bar on.

### Files

```bash
# terminal 1                                    # terminal 2
build/uninet-file-transfer receive ./incoming   build/uninet-file-transfer send report.pdf
```

```
  report.pdf: 100.0%  2.9 MB/2.9 MB
  report.pdf: saved to ./incoming/report.pdf (2.9 MB)
```

The Python version ([`python/file_transfer.py`](python/file_transfer.py)) does
the same and verifies a SHA-256 that travels as metadata.

### 3D volumes (numpy)

```bash
# terminal 1                                     # terminal 2
python3 examples/python/volume_transfer.py receive   python3 examples/python/volume_transfer.py send
```

A 256×256×256 float32 volume: 67 MB, with its shape, dtype and voxel spacing
carried alongside, so the receiver rebuilds the exact array:

```python
# sender
blob.send("scan-volume", volume, meta={
    "dtype": str(volume.dtype),
    "shape": list(volume.shape),
    "spacing": [0.5, 0.5, 1.0],
})

# receiver
volume = np.frombuffer(data, dtype=np.dtype(info.meta["dtype"]))
volume = volume.reshape(info.meta["shape"])
```

Nothing is agreed in advance and there is no side channel: the metadata is part
of the transfer.

> `np.ascontiguousarray(volume)` before sending. A sliced or transposed array is
> not contiguous, and the buffer protocol needs it to be.

### Meshes

```bash
# terminal 1                                   # terminal 2
python3 examples/python/mesh_transfer.py receive   python3 examples/python/mesh_transfer.py send
```

Streams a 4096-vertex surface at 20 Hz. Vertices are floats and take the
contiguous binary fast path; triangle indices are integers and **stay
integers**, which is why a list of ints is never silently converted to floats.

---

## What each example demonstrates

| example | discovery | presence | publish | addressed | Blob | metadata |
|---|:-:|:-:|:-:|:-:|:-:|:-:|
| `demo.cpp` | ● | ● | ● | | | |
| `python/basic.py` | ● | ● | ● | | | |
| `python/mesh_transfer.py` | ● | | ● | | | |
| `file_transfer.cpp` | ● | | | | ● | |
| `python/file_transfer.py` | ● | | | | ● | ● |
| `python/volume_transfer.py` | ● | | | | ● | ● |
| `csharp/UniNetDemo` | ● | ● | ● | ● | | |

`Blob` is available in all three languages (C++ `uninet::Blob`, Python
`uninet.Blob`, C# `UniNet.Blob`) and interoperates between them.

---

## Troubleshooting an example

If the two sides never find each other, the network is the cause, not the
example. Run the diagnostic:

```bash
build/uninet-discover --once
```

It lists what it can see and, when it sees nothing, says what to check. The
common causes are guest Wi-Fi with client isolation, a firewall blocking UDP
5670, and a machine with several networks where discovery picked the wrong one
(pass `--iface` to the C++ examples, `interface=` to `uninet.join()` in Python).

Examples use their own subjects and the default realm. If a real session is
running on the same network and you want the examples kept apart from it, pass
`--realm` (C++) or `realm=` (Python).

---

## Android / Meta Quest

[`android/`](android) is a complete demo app - a UniNet node running on the
device, discovering desktop peers over Wi-Fi or a USB cable. It is built in
three steps, from the repository root:

```bash
./scripts/build-for-android.sh          # the native library, arm64-v8a
./examples/android/build.sh --install   # the APK, installed on a connected device
./examples/android/usb-peer.sh          # a desktop peer reachable over USB
```

[`jni/uninet_jni.cpp`](android/jni/uninet_jni.cpp) is the JNI bridge, and
[`AndroidManifest.xml`](android/AndroidManifest.xml) shows the multicast
permission that Wi-Fi discovery needs on Android. See the README's
"Unity / Meta Quest" section for the same thing inside Unity.
