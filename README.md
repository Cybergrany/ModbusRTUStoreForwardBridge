# ModbusRTUStoreForwardBridge

`ModbusRTUStoreForwardBridge` is a small, allocation-free C++11 core for an
asynchronous Modbus RTU store-and-forward bridge. It deliberately does **not**
implement a transparent nested Modbus proxy. An upstream slave exposes a local,
flattened register image; accepted writes are captured as immutable work; a
separate downstream master services child endpoints later.

The library owns only reusable mechanics:

- fixed-capacity transactional ingress records;
- ordered upstream-to-endpoint route translation;
- non-owning cached register images;
- typed downstream requests, validation and completion records.

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

## Status

This repository is being prepared in an isolated local worktree for Stage C of
the OGM Modbus separation. A public remote and release tag will be assigned only
after the compatibility consumer and differential software gates are green.
