# ModbusRTUStoreForwardBridge

`ModbusRTUStoreForwardBridge` is a small, allocation-free C++11 core for an
asynchronous Modbus RTU store-and-forward bridge. It deliberately does **not**
implement a transparent nested Modbus proxy. An upstream slave exposes a local,
flattened register image; accepted writes are captured as immutable work; a
separate downstream master services child endpoints later.

The library owns only reusable mechanics:

- ordered upstream-to-endpoint route translation;
- non-owning cached register images;
- typed downstream requests, validation and completion records;
- consumption of caller-owned immutable ingress snapshots without taking
  ownership of queues, journals or buffers.

Product policy stays outside the library. In particular, this module has no
concept of OGM boards, generated children, gameplay state, load/reset/hash,
health counters, public ACK debt, retries, worker threads, serial ports, or
diagnostic logging. A product adapter decides when work runs and what a
completion means.

The first compatibility consumer is the existing OGM cached bridge. Its
migration contract is intentionally strict: register offsets, request/response
ADUs, admission-before-mutation ordering, immutable snapshots, downstream
message ordering and old slave firmware compatibility must remain unchanged.

Public API examples and invariants live beside each header under
`include/modbus_rtu_bridge/`. Native characterization tests live under `test/`.
`scripts/run_native_tests.sh` also runs a CPU-affined paired route-lookup gate;
the median neutral/legacy ratio may not exceed 1.05.

The extraction lineage and acceptance boundary are recorded in
[OGM_EXTRACTION_PROVENANCE.md](OGM_EXTRACTION_PROVENANCE.md).

## What the integration owns

The library is deliberately only the middle of a store-forward pipeline:

```text
upstream request
      |
      v
product admission/journal reservation -> visible cache + immutable snapshot
      |
      v
RouteTableView (prove whole downstream span) -> product bounded queue
      |
      v
DownstreamExecutor -> product result policy
                                                  |
                                                  v
                         markApplied / restore + ACK/fail/retry/recovery
```

The caller must preserve admission-before-mutation, complete-range validation,
source ordering and the lifetime of every borrowed route, cache and request
buffer. The library does not add a thread, a mutex, a retry or a serial write.
That makes it possible for an existing runner to retain its established
scheduler and wire timing while delegating the neutral calculations.

## Installation

Version `0.1.0` is the first Stage C hardware-accepted compatibility release.
Version `0.1.1` retains the same public header implementation and adds the MIT
license/release metadata. During local migration work, use an explicit path
dependency so the module and consumer are tested together:

```ini
lib_deps =
  symlink:///absolute/path/to/ModbusRTUStoreForwardBridge
```

Remote consumers must pin the published `v0.1.1` tag or its full resolved
commit rather than a moving branch:

```ini
lib_deps =
  https://github.com/Cybergrany/ModbusRTUStoreForwardBridge.git#v0.1.1
```

Include only the contracts the adapter uses:

```cpp
#include <modbus_rtu_bridge/RouteTable.h>
#include <modbus_rtu_bridge/CacheImage.h>
#include <modbus_rtu_bridge/DownstreamExecutor.h>
```

All current headers are C++11, allocation-free and header-only. Storage,
synchronization and platform services remain caller-owned.

## Validation

Run the standalone native suite from the repository root:

```bash
scripts/run_native_tests.sh
```

The script compiles the API suite as strict GNU C++11 at `-Os`, then runs a
paired `-O2` route-lookup comparison. When `taskset` is available it pins the
comparison to the first allowed CPU; `OGM_MODBUS_PERF_CPU=<cpu>` selects an
explicit allowed CPU. The paired median candidate/legacy ratio must be at most
`1.05`.

This gate checks deterministic API behavior and one synthetic host hot path.
It does not execute an Arduino UART, the OGM worker queues, mbed scheduling,
RS485 direction timing, serial drain, full bridge ADUs or bus contention. The
consumer still needs differential ordering/trace tests, exact embedded builds,
footprint review and physical RS485 validation with the deployed unchanged
slave firmware.

## Status

Version `0.1.0` closes Stage C of the OGM Modbus separation. The exact
behavior-bearing module source accepted on hardware was
`d15d9474be43205c4149a69c345d07c92cc2d098`, consumed by `OGM_bridge`
`59311ac8027cd3a8f28ae40d6253a285c4b62224`. On 2026-08-25 that bridge ran
against the unchanged deployed slave firmware with established-variance
parity in the corrected 34.94-minute soak and no gameplay-visible regression.

The `v0.1.0` tag may point at a later documentation-only release commit. That
does not imply that a different implementation was tested: the hardware-tested
library source remains the exact commit above. See
[RELEASE_NOTES.md](RELEASE_NOTES.md) and
[OGM_EXTRACTION_PROVENANCE.md](OGM_EXTRACTION_PROVENANCE.md) for the software
and physical acceptance boundary.

This acceptance applies to the asynchronous cached store-forward contract. It
must not be presented as validation of a drop-in transparent Modbus proxy.

Version `0.1.1` adds MIT licensing and corrects package metadata and archive
contents without changing the implementation accepted as `v0.1.0`.
