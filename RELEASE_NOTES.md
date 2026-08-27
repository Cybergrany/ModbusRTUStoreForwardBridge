# Release notes

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
copy.

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
