# OGM extraction provenance

`ModbusRTUStoreForwardBridge` is a new neutral module, not a fork of a CMB27
repository and not a transparent Modbus gateway. Its contracts were extracted
from the existing OGM asynchronous cached store-forward bridge while that
product remained the behavioral reference.

## Extraction anchors

| Purpose | Repository / commit |
| --- | --- |
| Frozen OGM bridge behavior used as the Stage C production baseline | `OGM_bridge` `fcdb9c9a1fcbd6d14a8845f7253107334807a525` |
| Software-validated neutral slave ingress seam consumed by the bridge line | `OGM_slave_core` `6a6d7fed95dc66d5b3c939c2cc8b7789596dddf0` |
| Static master ownership seam used by the candidate dependency tree | `OGM_Portable` `2055adb449c1e767217f09f99efda32e52a0306d` |
| Root of this new module's independent history | `b41be50` (`Initialize store-forward bridge module`) |

The short root ID is unambiguous within this new repository; release records
must use the full resolved commit and exact dependency locks. The initial
bridge extraction used a reviewed vendored snapshot while the repository was
being prepared. The accepted consumer now resolves the published module by
its immutable full commit, so its firmware build does not depend on a moving
remote branch.

## Stage C hardware-acceptance anchors

| Purpose | Repository / commit |
| --- | --- |
| Exact behavior-bearing module source exercised on hardware | `ModbusRTUStoreForwardBridge` `d15d9474be43205c4149a69c345d07c92cc2d098` |
| Exact bridge consumer exercised on hardware | `OGM_bridge` `59311ac8027cd3a8f28ae40d6253a285c4b62224` |
| Downstream compatibility target | Existing deployed OGM slave firmware, unchanged for the Stage C run |

The user accepted Stage C on 2026-08-25 after static/native/performance gates,
gameplay observation and the corrected 34.94-minute bridge-only soak all showed
behavior within the established variance. The `v0.1.0` tag may include this
documentation-only closeout after `d15d9474`; it does not identify a second,
separately hardware-tested implementation. Release audits must retain both the
tag target and the exact behavior-bearing commit above.

## Preserved product semantics

The OGM reference is asynchronous cached store-forward:

- the upstream slave admits a write locally, mutates the flattened image and
  captures immutable work before replying;
- downstream child operations execute later through the product's bounded
  queues and worker;
- upstream reads observe a flattened cached image, not a synchronous child
  response;
- OGM owns child/session generations, activation/load/reset/hash, health,
  public ACK/fail debt, retries, recovery and FC 0x45 latest-state behavior.

The neutral module may own route/range translation, non-owning cache operations
and standard downstream dispatch contracts. It must not reinterpret the above
policy or introduce `externalIOBoards`, generated-child types, gameplay state,
mbed threads, Arduino serial or product logging into its public headers.

## No-wire-delta rule

The extraction is acceptable only if the existing consumer retains:

- flattened register addresses and overlays used by old master/slave firmware;
- complete-range validation before any cross-child partial dispatch;
- reserve -> mutate -> immutable snapshot -> upstream reply ordering;
- source-token and downstream request ordering across tables and rollover;
- exact standard function selection and product-owned FC 0x45 behavior;
- retry/timeout/late-result, ACK/fail, disconnect and cache-rollback semantics;
- existing worker, queue, lock, serial-drain and RS485 direction timing;
- bounded memory use and accepted route/executor performance.

A synchronous per-unit proxy, changed register map or updated slave protocol is
outside this extraction and requires a separate migration. Deployed original
OGM slave firmware is an explicit compatibility target and cannot be assumed
updatable.

## Validation boundary

The standalone suite validates deterministic neutral API behavior and compares
route lookup with the legacy search shape on a pinned host CPU. The OGM bridge
characterization suite freezes flattened cache, admission, snapshot, ordering,
range, retry, timeout, reconnect and counter-rollover semantics in a separate
fixed-capacity reference.

Those tests did not by themselves prove that the production adapter delegates
every path correctly, nor did they exercise UART interrupts, mbed scheduling,
serial drain, RS485 turnaround, T3.5 gaps or contention on the physical bus.
The exact consumer tree therefore also passed its differential software gates
before the bridge-only physical run described above. That run found no
behavioral or gameplay-visible regression while retaining unchanged deployed
slaves, and Stage C is accepted for the exact commits recorded here.

This is compatibility evidence for the established OGM store-forward product,
not proof of every possible bus topology, electrical fault or synchronous
proxy use. Future behavior-bearing changes require their own proportional
software and hardware validation; documentation-only release metadata does
not broaden the tested claim.
