# UniNet

**Unified Networking transport** — one versioned pub/sub wire protocol for
distributed medical-navigation peers, with a compiled C++ core (CBOR codec,
negotiated compression, pluggable transport, high-level Node) and C# / Python
bindings. One codec, one framing, one bus — owned by this project; consumers
depend on UniNet rather than each re-implementing the bus.

It exists because an audit of ThermoNavMR (C#), ThermoNavServer (C++) and
ThermoNavSlicer (Python) found the **same** NATS+CBOR protocol re-implemented
**three times**, with hand-mirrored schemas and LZ4 code that nobody could enable
(no peer knew how a frame was encoded). UniNet is that bus, written once.

## What it is

A peer (`Node`) publishes/subscribes `Envelope`s over a pluggable `Transport`. The
envelope carries who-sent / who-for / subject / payload; the payload is an
arbitrary `Cbor` value. The codec, compression and protocol filters (echo
suppression, dst targeting, reconnect) live here, once — what each ThermoNav peer
hand-rolls today (`Networking.cs`, `networking.cpp`, `networking.py`).

- **v0.1 (current):** dependency-free CBOR codec + negotiated compression
  (none/zlib/lz4) + `LoopbackTransport` + `Node` + C ABI + Python + C# bindings.
- Layered so a new transport (NATS, mesh, BLE) is an addition, not a rewrite.

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the canonical wire standard.

## Installation

**One command does everything** — installs prerequisites, builds the C++ core, the
C ABI (for C#), optionally the Python extension, and runs the tests:

```bash
./scripts/bootstrap.sh --python     # Linux / macOS (use scripts/bootstrap.ps1 on Windows)
```

Prefer the manual route? It's plain CMake. Prerequisites: a **C++17** compiler,
**CMake ≥ 3.18**, and **zlib** + **liblz4** (lz4 is optional — auto-detected).

| OS | install prerequisites |
|---|---|
| **Ubuntu/Debian** | `sudo apt install build-essential cmake pkg-config zlib1g-dev liblz4-dev` |
| **Fedora** | `sudo dnf install gcc-c++ cmake pkgconf-pkg-config zlib-devel lz4-devel` |
| **Arch** | `sudo pacman -S base-devel cmake pkgconf zlib lz4` |
| **macOS** | `brew install cmake pkg-config zlib lz4` |
| **Windows** | `scripts/bootstrap.ps1` (auto-clones vcpkg for zlib + lz4); needs Git, CMake, and VS 2022's "Desktop development with C++" workload |

Then build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DUNINET_BUILD_CABI=ON
cmake --build build -j
ctest --test-dir build                                   # codec / compression / pub-sub tests
```

On **Windows**, point CMake at the vcpkg toolchain:
```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
      -DUNINET_BUILD_CABI=ON
cmake --build build --config Release
ctest --test-dir build -C Release
```

CMake options (all optional, auto where possible):
- `UNINET_LZ4=ON` — LZ4 tier (auto-detected via pkg-config/vcpkg). Falls back to zlib + none.
- `UNINET_NATS=ON` — builds `NatsTransport` (fetches cnats; the production brokered backend). Off by default.
- `UNINET_BUILD_CABI=ON` — `libuninet_c.so/.dll/.dylib` (the C ABI for C# / P-Invoke).
- `UNINET_BUILD_PYTHON=ON` — the pybind11 extension (set automatically by `pip install`).

### Python

```bash
pip install .                      # builds the C++ extension via scikit-build-core
# editable:
pip install -e .
```

```python
import uninet
bus = uninet.LoopbackTransport(); bus.connect()
a = uninet.Node("alice", bus); b = uninet.Node("bob", bus)
a.connect(); b.connect()
b.subscribe("domain.D1", lambda env: print("got", env.data["text"].as_text()))
a.publish("domain.D1", uninet.Cbor.map().set("text", uninet.Cbor.text("hi")))
# -> got hi
```

### C# / Unity (HoloLens)

1. Build the C ABI with `-DUNINET_BUILD_CABI=ON` → produces `libuninet_c.so` /
   `uninet_c.dll` / `libuninet_c.dylib`.
2. Reference `csharp/UniNet/UniNet.csproj` (P/Invoke) from your app, or run the demo:
   ```bash
   cd csharp/UniNetDemo && dotnet run        # after `dotnet build` (place the native lib next to the exe)
   ```
3. For Unity/HoloLens: drop the native lib (an **ARM64** build for the headset) into
   your `Assets/Plugins/` folder. This is the binding the MR headset needs and that
   UniVox never had.

```csharp
using var bus = new LoopbackTransport();
using var a = new Node("alice", bus);
using var b = new Node("bob", bus);
b.Subscribe("domain.D1", (subj, text) => Console.WriteLine($"{subj}: {text}"));
a.Publish("domain.D1", "hello from C# over UniNet");
```

### Troubleshooting
- **`'pybind11/pybind11.h' not found`** — `pip install pybind11`, then pass
  `-Dpybind11_DIR=$(python3 -m pybind11 --cmakedir)` (the bootstrap script does this for you).
- **LZ4 not detected** — install `liblz4-dev` / `lz4-devel` / `brew install lz4`; otherwise
  UniNet builds with zlib + none (the `Lz4` rows are simply omitted from the benchmark).
- **`libuninet_c.so: cannot open shared object file`** (C#/Python) — add its directory to
  `LD_LIBRARY_PATH`, or on Windows place `uninet_c.dll` next to your executable.

## Layout

```
include/uninet/   types.h · cbor.h (codec) · codec.h (envelope+compression)
                  · transport.h · loopback.h · node.h · nats_transport.h
                  · cabi.h (C ABI) · profiler.h
src/              cbor · codec · loopback · node · profiler · nats_transport · cabi
python/           bindings.cpp (pybind11) · uninet/ (package)
csharp/           UniNet/ (P/Invoke wrapper) · UniNetDemo/ (demo)
scripts/          bootstrap.sh (Linux/macOS) · bootstrap.ps1 (Windows)
tests/            round-trip + pub/sub correctness (C++) · benchmark.cpp
docs/             PROTOCOL.md (canonical wire spec) · benchmark_*.png
tools/            plot_benchmark.py
```

## Performance & diagnostics

UniNet ships the same opt-in profiler UniVox uses (`uninet.profiler`): a zero-cost
`ScopedOp` RAII timer placed at the **operation** level of every hot path
(`cbor.encode`, `cbor.decode`, `compress.*`, `decompress.*`, `frame`, `unframe`,
`node.publish`, `loopback.deliver`). Enable it, run a workload, read the per-op
breakdown sorted by total time — the dominant cost is always at the top.

The benchmark is a **collect-then-plot pipeline**: it runs the full pipeline matrix
(encode → compress → frame → deliver → unframe → decode) at three mesh sizes,
**appends every run to `uninet_bench_log.csv`** (so runs accumulate for before/after
comparison), and dumps the profiler report to `uninet_profile.txt` with a
`DOMINANT op` callout pointing at the next lever to pull.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j --target benchmark
./build/benchmark 200 2000          # CSV to stdout + uninet_bench_log.csv + uninet_profile.txt
python3 tools/plot_benchmark.py     # reads the CSV -> regenerates docs/benchmark_*.png
```

![Codec + transport throughput](docs/benchmark_throughput.png)

![Compression tiers](docs/benchmark_compression.png)

4096-vert mesh (~108 KiB uncompressed CBOR, mean of 200 reps, 32-core box):

| op | throughput | notes |
|---|---|---|
| **encode** | **8.1 GB/s** | bulk-write fast path (was 1.3 GB/s — see below) |
| **decode** | **7.9 GB/s** | memory-bandwidth-bound (bulk float-array fast path) |
| **frame** | 5.0 GB/s | encode + compress |
| **unframe** | 4.2 GB/s | decompress + decode |
| **LZ4 compress** | 14.8 GB/s (ratio 1.42×) | see the caveat below — this figure is an artifact |
| **LZ4 decompress** | 12.3 GB/s | |
| zlib compress | 36 MB/s (ratio 2.29×) | 108 KiB → 47 KiB — best ratio, too slow for live |
| zlib decompress | 504 MB/s | |
| **loopback pub/sub** | **23.1k msgs/s · 2.44 GB/s** | full encode→unframe→dispatch round-trip |

> **Two corrections to earlier numbers in this table.**
>
> **1. The 27.8k msgs/s previously published here was not reproducible.** It came
> from a single lucky run: glibc raises its mmap threshold after it sees large
> blocks freed, so that one run happened to serve the framing buffers from the
> heap instead of via `mmap`. Every subsequent run in `uninet_bench_log.csv`
> measured **5.5–5.9k msgs/s** — a 4.7× gap nobody noticed because the README was
> frozen at the best result. The cause was per-message allocator churn (§ below);
> it is now fixed, and 23.1k is what the benchmark reproduces run over run.
>
> **2. The LZ4 compression figures are measured on a linear ramp**
> (`pts[i] = i * 0.123f`), which is far more compressible than real mesh
> coordinates. 14.8 GB/s is above what single-core LZ4 can do on genuine data
> (~1 byte/cycle ⇒ 3–5 GB/s), and the 1.42× ratio will be closer to 1.0–1.05×
> on real float32 point clouds. Treat "LZ4 is free, leave it on" as unproven
> until the benchmark carries a realistic payload.

**Two profiler-driven wins so far (loopback 8.5k → 27.8k msgs/s = 3.3×):**

1. **`cbor.encode` 6× win.** The first profile flagged encoding as the dominant cost —
   1.3 GB/s vs decode's 7.8 (6× slower), 86% of framing time. Cause: the float-array
   path did one `push_back` per byte (61k calls + reallocs) while decode read bulk.
   Switching encode to pre-size and write directly lifted it to **8.1 GB/s** and
   doubled loopback (every publish pays the encode cost).

2. **Echo suppression without decompress.** The next profile showed
   `unframe`/`decode`/`decompress` running **4000× for 2000 publishes** — each
   publisher was decoding its *own echo* then discarding it, and with compression on
   it had to decompress first just to read `src`. Fix: put `src`/`dst`/compression in
   a **clear binary header** before the compressed core (`peek_routing`), so
   `Node::on_raw_` drops echoes *before* touching the payload. `unframe` calls halved
   (4k → 2k) and loopback rose another **+45%** (17.7k → 27.8k).

**3. Allocator churn — the win the profiler could not see (3.9×).**

After the first two wins the per-op table looked *balanced*, which was read as
"the easy wins are exhausted." It wasn't: the dominant cost was **inside malloc**,
where no `ScopedOp` reaches. Every `publish` allocated and freed three buffers
(encoded core → compressed payload → wire) and every receive allocated another.
glibc's mmap threshold starts at 128 KiB, so for the mesh payloads UniNet actually
carries (~100–450 KiB) each of those was an `mmap`/`munmap` syscall pair plus page
faults on first touch — and `munmap` cannot recycle the block, so the next message
paid it all again.

The fix is Cornflakes-style arena reuse: `frame_into`/`unframe_into` take a
caller-owned `Scratch`, `Node` keeps one per thread, and `LoopbackTransport` keeps
one delivery buffer per re-entrancy depth. Steady-state publishing now allocates
**nothing** after the first message.

| payload | before | after | |
|---|---|---|---|
| 512 verts (13.8 KiB) | 43.3k msgs/s | **72.7k** | 1.68× |
| **4096 verts (108 KiB)** — the live MR mesh size | 5.9k msgs/s | **23.1k** | **3.9×** |
| 16384 verts (432 KiB) | 3.4k msgs/s | 3.7k | 1.06× |

The 432 KiB row barely moves because at that size the codec itself is
memory-bandwidth-bound — the allocator was never the dominant term there. The win
is concentrated exactly where ThermoNav operates.

*Lesson for the profiler:* an op-level breakdown attributes allocator time to
whichever op happened to call `malloc`, so a cost that is spread evenly across
every op reads as "balanced" rather than "dominant." Balance is not proof that the
wins are exhausted.

**What the tier numbers say:**
- **LZ4 is essentially free and should stay on for every frame** — 14.6 GB/s with a
  real 1.4× cut. This is the lever the three ThermoNav peers *already coded* but
  force-disabled to `NONE` everywhere (no peer could tell how a frame was encoded);
  UniNet's 1-byte compression header fixes that.
- **zlib's better ratio (2.3×) isn't worth it live** — 36 MB/s compress is ~400×
  slower than LZ4. Reserve zlib for archival/batch, LZ4 for the OR.
- The protocol layer is not the bottleneck at any realistic rate (the MR peer
  throttles to ~20 Hz today; UniNet sustains ~17k pub/sub round-trips/s).

Compression level is tunable at runtime (`uninet.set_compression_level(1..9)` for
zlib; default 6).

## Hostile-input hardening

Frames arrive from the network, so every length prefix in them is attacker
controlled. Three defects made a peer killable by a tiny frame; all three are
fixed and covered by `test_hostile_frames`:

| Defect | Trigger | Was |
|---|---|---|
| Length-prefix overflow | 9-byte frame declaring a 2⁶⁴−1 byte string — `c.i + n` wraps, so the bounds check passed | `std::length_error` → **process abort** |
| Stride overflow in the float fast path | array count `n` where `n*5` wraps to a small value | heap out-of-bounds read |
| Unbounded decode recursion | ~8.5k nested indefinite-length arrays, which LZ4-compress to **~60 bytes** | stack exhaustion → **segfault** |

Lengths are now range-checked without overflowing (`fits()`) and decode depth is
capped at 128. There is still **no authentication or integrity check on the wire**
— `src_uuid` is self-asserted plaintext, so echo suppression and `dst` targeting
are spoofable by any peer on the bus. That is the outstanding design gap.

**Also fixed — silent data loss on high-ratio payloads.** `decompress` used to
guess the output size from the input size and grow, giving up after 8 (zlib) or
32 (LZ4) attempts; past that ceiling it returned empty, `unframe` returned
`nullopt`, and `Node` dropped the message **with no error anywhere**. Anything
compressing better than ~190× was lost — including sparse `safety_map` payloads,
a documented ThermoNav message type:

```
             before          after
128x128      OK              OK
256x256      *** DROPPED *** OK      (ratio 224x)
512x512      *** DROPPED *** OK      (ratio 234x)
```

LZ4 now reads the true content size from the frame header instead of guessing.

## Status (v0.1)

**Verified:** CBOR round-trips (all kinds, incl. fast float arrays), zlib + LZ4
compression round-trips, envelope frame/unframe, loopback pub/sub (echo suppression,
dst targeting, wildcard), reconnect-with-backoff, synchronous request-reply
(`Node::request` over a request-capable transport), profiler, Python bindings
(`pip install`), C ABI + C# wrapper, builds CPU-only with no broker required.

**Deployed:** all three ThermoNav peers now consume UniNet instead of their
hand-rolled buses — ThermoNavServer (vendored C++ core + `networking.cpp`),
ThermoNavSlicer (Python extension + `networking.py`), ThermoNavMR (C ABI /
P-Invoke + `Networking.cs`). Each kept a `.legacy` copy of the code it replaced.
The application message taxonomy is **not** owned here — it lives in one IDL
(`ThermoNavServer/prototypes/comm_standard/schema.toml`), which generates the
bindings for every peer; UniNet stays schema-agnostic.

**Staged (documented, not yet in-tree):** the `NatsTransport` wired into a live
benchmark against a broker; mesh discovery + BLE backends behind the `Transport`
interface; request-reply exposed through the C ABI / C# wrapper (today it is C++
and Python only); a managed CBOR surface for C#; and cross-platform wheels via CI
(`cibuildwheel`) plus a NuGet package.

## License

MIT.
