# Release notes

## v0.3.2

`PollIndexScan` now stores its scalar scan/commit state directly instead of
embedding the general-purpose planner state. Its public API and decisions are
unchanged. The smaller implementation reduces stack use and gives compilers a
clearer hot path for integrations that only need round-robin indices.

## v0.3.1

Added `PollIndexScan`, a small allocation-free wrapper for integrations that
store only a round-robin child index and last-poll timestamp. It preserves the
existing `PollPlanner` scan and commit rules: declined or abandoned candidates
do not advance persistent state, invalid indices fail closed, and a changed
candidate count rejects the commit. Native tests compare its result and cost
with the equivalent lower-level planner sequence.

## v0.3.0

Added an optional cooperative orchestration layer while keeping the lower-level
planning APIs unchanged:

- caller-backed `FixedRingQueue<T>` storage with bounded capacity and wraparound
  behavior;
- a multi-work `CompletionLedger` that records one exact outstanding request
  per work and preserves applied, definitely-not-sent, uncertain-send and
  terminal-failure results;
- configurable round-robin poll fairness with separate preview/commit, a
  low-RAM index scan, candidate-count-bound selections, no-dispatch poll
  consumption and caller-owned time;
- one heterogeneous `StoreForwardBridge` FIFO for coil and holding-register
  work, with one request sequence/fairness domain, exact action validation and
  one-at-a-time retirement that returns caller snapshot context;
- an optional template adapter for the conventional `ModbusRTUMaster` API; and
- native gates for full/wrapped storage, capacity exhaustion, cross-table
  ordering, stale generations, mutated actions, failure-stop behavior,
  starvation/decline paths, zero allocations, object size and cooperative-cycle
  performance.

The facade remains synchronous and caller-driven. It adds no worker, clock,
serial I/O or retry loop. Payload snapshots stay caller-owned and immutable.
`nextAction()` does not advance state; `admitAction()` is the publication point;
`complete()` accepts one terminal post-retry result. A terminal failure stops
unissued fragments and never rolls desired cache state back automatically.
For an otherwise valid active completion, the existing direct
`resolveCompletion()` helper now rejects unknown `DownstreamOutcome` values
with `InvalidOutcome` before mutating its aggregate or emitting a cache/notice
decision. A stale-session facade result may report `workRetired` alongside its
pre-abandon ledger summary; `workRetired` is the current lifetime fact.

## v0.2.0

Added allocation-free planning contracts for building a cached forwarder:

- explicit tracked, silent-ordered, latest-state, and rejected ingress modes;
- immutable snapshot views with wrap-safe work and session identities;
- complete-range preflight and deterministic endpoint/chunk request planning;
- ordered aggregate completion decisions that never roll back newer desired
  data; and
- trace-style native tests for ordering, rollover, failures, and source
  offsets, plus a paired forward-planning performance budget.

Existing route, cache, and downstream-executor APIs are unchanged. The new
planner does not add a queue, worker, clock, retry, serial operation, or payload
copy. Unknown ingress-delivery and span-policy enum values fail closed rather
than being treated as accepted or splittable work.

## v0.1.2

Corrected package documentation. Public headers are unchanged from v0.1.1.

## v0.1.1

Added MIT licensing, repository metadata, public-header declarations, and
package export rules. Public headers are unchanged from v0.1.0.

## v0.1.0

Initial release of the allocation-free C++11 store-and-forward core:

- validated upstream-to-endpoint route translation;
- visible and applied cache images;
- typed downstream requests, validation, and completions; and
- native behavior and route-lookup performance tests.

The library is an asynchronous cached core, not a transparent Modbus proxy.
Serial I/O, queues, scheduling, retries, and completion policy remain with the
application.
