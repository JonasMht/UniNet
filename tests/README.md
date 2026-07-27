# Tests

Run everything that works on this machine:

```bash
./scripts/test-all.sh              # native suites
./scripts/test-all.sh --docker     # plus cross-platform and cross-language
```

A stage that cannot run reports SKIP, never PASS. An untested thing is not a
working thing.

## Layout

| path | what it is | how to run |
|---|---|---|
| `test_roundtrip.cpp` | codec: CBOR round-trips, compression, framing, hostile frames | `ctest --test-dir build -L uninet -R roundtrip` |
| `test_network.cpp` | the network: discovery, presence, broadcast, unicast, realms, gossip, threading, large payloads, close | `ctest --test-dir build -L uninet -R network` |
| `test_cabi.c` | the C ABI, compiled **as C**, the path P/Invoke takes | `ctest --test-dir build -L uninet -R cabi` |
| `interop/` | one C++, one Python and one C# node in a realm, each checking the others' payloads | `./scripts/test-interop.sh` |
| `docker/` | cross-platform images (see below) | `./scripts/test-all.sh --docker` |
| `benchmark_codec.cpp` | codec throughput in isolation | `build/uninet-benchmark-codec` |
| `benchmark_network.cpp` | end-to-end: discovery latency and message throughput | `build/uninet-benchmark` |
| `../python/tests/` | Python bindings: dicts, numpy, discovery, threading, shutdown | `pytest python/tests` |

## Conventions

**Every network test uses a realm unique to its process.** A demo running on the
same machine, or a second CI job on the same build box, cannot perturb a run.

**Timing is tolerant, outcomes are strict.** Discovery is a network event, not a
function call, so tests poll with a generous deadline rather than asserting
immediately. What they assert on is exact.

**Tests are written to fail loudly for the right reason.** Where a limitation is
known and external (Wine's network-interface enumeration, a missing runtime),
the runner reports it as SKIP or EXPECTED STOP with the reason, so nobody reads
it as coverage.

## Cross-platform images

| image | verifies |
|---|---|
| `Dockerfile.linux` | Debian with Zyre built from source, the path a machine without a system Zyre takes, running C++, Python and C# |
| `Dockerfile.windows-check` | every translation unit compiles for `x86_64-w64-mingw32` at `-Werror` |
| `Dockerfile.windows-run` | the cross-compiled `.exe` files actually run, under Wine |

The Windows images exist because Windows-only defects are invisible on Linux
until someone tries. They are what caught `interface` being a macro in
`<objbase.h>` and `gmtime_r` not existing on MSVC.

Under Wine, `test_roundtrip.exe` passes in full and `test_cabi.exe` passes its
JSON, CBOR and null-safety sections as genuine Windows code. Discovery is not
covered: Wine does not implement the `GetAdaptersAddresses` buffer-sizing
protocol that czmq asserts on. Closing that gap needs a real Windows machine,
which is what the `windows:msvc` job in `.gitlab-ci.yml` is for.

## Sanitizers

```bash
./scripts/test-all.sh --sanitizers
```

**Build the dependencies under the sanitizer too.** Against a system libzmq that
ThreadSanitizer cannot see into, a run reported 47 races: all but one were
synchronisation inside an uninstrumented library. With `-DUNINET_SYSTEM_ZYRE=OFF`
so the whole stack is instrumented, the same run reports zero. A TSan result
against uninstrumented dependencies is not evidence of anything.

The one real race it did find was `Node::ensure_watching_`, where two threads
calling `subscribe()` could each install the internal subscription and every
message was then delivered twice.

Run them by hand when changing the transport or the codec:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"
cmake --build build-tsan -j && ./build-tsan/test_network

cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g -O1"
cmake --build build-asan -j && ./build-asan/test_roundtrip
```

Both also run in CI on every push.
