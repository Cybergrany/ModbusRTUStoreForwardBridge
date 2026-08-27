# Public API guide

All APIs in this directory are allocation-free, C++11-compatible and
platform-neutral. They operate on caller-owned storage and perform no serial,
thread, clock or logging operations.

## Ownership and concurrency

| Contract | Storage owner | Hidden state/allocation | Synchronization |
| --- | --- | --- | --- |
| `RouteTableView` | Caller owns immutable `EndpointRoute[]`. | None. A `RouteCursor` is caller-owned iteration state. | Caller prevents route mutation while any lookup/cursor may run. |
| `MutableImageView<T>` | Caller owns the visible/applied arrays. | None. The view is only a pointer and count. | Caller retains the existing cache lock around each logically atomic operation. |
| `IngressWorkView<T>` | Caller owns the immutable payload for the full work lifetime. | None. The view stores identity, range, delivery, pointer, and count. | Admission, publication, and payload lifetime belong to the caller. |
| `ForwardCursor<T>` / `PlannedWriteRequest` | Caller owns cursor, request, route storage, and borrowed payload. | None. Planning reads the snapshot in place. | Do not mutate topology or payload until all planned requests finish. |
| `CompletionAggregate` | Caller owns one aggregate per logical work item. | Two serials and bounded scalar state; no fragment allocation. | Resolve completions in planner order under the caller's queue/cache policy. |
| `DownstreamRequest` | Caller owns request and pointed-to buffers. | None. `consistencyContext` is an untyped borrowed hook. | Queue, request-buffer lifetime and any backend lock belong to the adapter. |
| `DownstreamCompletion<Result>` | Returned by value. | None. | Delivery/order belong to the runner. |

No contract makes a borrowed pointer safe after its owner returns, resizes,
rebuilds topology, or reuses a queue slot. A threaded adapter must define and
preserve publication and lock ordering around every borrowed object.

## Route tables

Build one `EndpointRoute` per downstream unit. Ranges are half-open and every
register table's upstream starts must stay in route order, including entries
whose count is zero.

```cpp
#include <modbus_rtu_bridge/RouteTable.h>

using namespace ModbusRTUBridge;

EndpointRoute routes[2];
routes[0].endpointId = 4;
routes[0].upstream[2] = AddressRange(100, 4);   // holding 100..103
routes[0].downstream[2] = AddressRange(0, 4);  // child holding 0..3
routes[1].endpointId = 9;
routes[1].upstream[2] = AddressRange(104, 3);
routes[1].downstream[2] = AddressRange(20, 3);

RouteTableView table(routes, 2);
if(table.validate() != RouteTableStatus::Valid){
  // Fail setup before accepting upstream traffic.
}

RouteCursor cursor;
if(table.beginSpan(RegisterTable::HoldingRegisters, 102, 5, cursor) ==
   RoutedSpanStatus::Complete){
  RouteSegment segment;
  while(table.next(cursor, segment)){
    // First: endpoint 4, downstream 2, count 2, snapshot offset 0.
    // Second: endpoint 9, downstream 20, count 3, snapshot offset 2.
  }
}
```

`beginSpan()` proves complete coverage before returning the first segment. This
is important for store-forward bridges: a frame containing an unmapped gap must
not be partially dispatched and then rejected.

`validate()` is a required setup gate, not an optional diagnostic. Do not call
`locate()`, `inspectSpan()` or `beginSpan()` on a table that has not returned
`RouteTableStatus::Valid` since its route storage was last changed.

Ordering includes zero-count mappings. For each register table, a zero-count
`upstream` range must retain the sorted insertion-point address where that
endpoint would occur; its `downstream.count` must also be zero. The empty range
never matches an address, but its meaningful start lets sparse lookup remain
logarithmic instead of falling back to a linear scan. Do not leave an
arbitrary/default zero start between later
populated mappings.

Call `validate()` once after constructing/rebuilding topology and fail setup on
any status other than `Valid`:

| Status | Meaning |
| --- | --- |
| `NullStorage` | A non-zero route count was paired with a null route pointer. |
| `RangeOverflow` | An upstream or downstream half-open range extends past address `65535`. |
| `CountMismatch` | An endpoint's upstream and downstream counts differ for one table. |
| `Unsorted` / `Overlap` | Route starts/ranges violate the deterministic sparse lookup order. |

`inspectSpan()` / `beginSpan()` additionally distinguish an empty request,
16-bit address overflow and an unmapped gap. A `Complete` result means the
whole requested span is covered at inspection time; it does not make mutable
route storage immutable.

Route storage is borrowed and must not be mutated while cursors are active.
Routes describe address translation only. Whether an endpoint is loaded,
healthy, in the current session, or eligible for fire-and-forget coalescing is
product policy and intentionally absent.

## Cached images

`MutableImageView<T>` is a bounds-checked borrowed span. Pair the visible
upstream image with the last applied downstream image using
`DesiredAppliedCache<T>`:

```cpp
#include <modbus_rtu_bridge/CacheImage.h>

bool upstreamCoils[32] = {};
bool downstreamApplied[32] = {};
ModbusRTUBridge::DesiredAppliedCache<bool> coils(
    ModbusRTUBridge::MutableImageView<bool>(upstreamCoils, 32),
    ModbusRTUBridge::MutableImageView<bool>(downstreamApplied, 32));

const bool admittedSnapshot[3] = {true, false, true};
coils.captureDesired(8, admittedSnapshot, 3); // local accepted state

// After downstream success:
coils.markApplied(8, admittedSnapshot, 3);

// After a later strict write fails and caller-owned generation metadata proves
// this range has not been superseded:
coils.restore(8, 3); // visible image returns to last applied state
```

The caller defines the lock scope. Copy methods do not lock, and source and
destination ranges must not overlap.

A typical product sequence is:

1. Reserve enough ingress/journal capacity for every eventual route segment.
2. Under the existing cache lock, copy the already-validated immutable ingress
   snapshot into `captureDesired()`.
3. Publish the snapshot/work record using the product's established ordering.
4. After downstream success, call `markApplied()` under that same cache policy.
5. On a product-defined rollback path, first prove the range is still owned by
   that work, then call `restore()` for exactly the failed range. The core does
   not decide whether a result is terminal or retryable.

All copy helpers fail closed on a null pointer, zero count or out-of-range
span. They return `false`; they do not truncate a request or partially copy it.

## Ingress work and sessions

`IngressWorkView<T>` describes one accepted upstream write without owning its
payload. The payload is immutable until every request planned from it has
finished.

| Delivery | Ordering | Public completion |
| --- | --- | --- |
| `Tracked` | Preserve source order. | Exactly one success or failure is owed. |
| `SilentOrdered` | Preserve source order. | None, while the downstream result still matters. |
| `LatestState` | May be coalesced by the caller. | None. |
| `Rejected` | Must not be planned. | None. |

`LatestState` describes queue semantics, not a downstream Modbus function. A
runner may still emit only standard function codes 5, 6, 15, and 16.

```cpp
#include <modbus_rtu_bridge/IngressWork.h>

using namespace ModbusRTUBridge;

const uint16_t snapshot[3] = {10, 20, 30};
const WorkIdentity identity(7, 2);  // non-zero token and session generation
const HoldingIngressWorkView work = makeHoldingIngressWork(
    identity, 102, 3, IngressDelivery::Tracked, snapshot);

const SessionStateView session(true, 2, 2, SessionPhase::Ready);
if (admissionOpen(session) && workCurrent(work.identity, session)) {
  // Publish work using caller-owned queue or cooperative state.
}
```

`nextNonZeroSerial16()` reserves zero as invalid. `nonZeroSerial16Before()` is
wrap-safe while compared live tokens remain less than half the 16-bit serial
space apart. Tokens from different sessions are deliberately unordered.

A basic cooperative planner may use `WorkIdentity()` when it already owns all
ordering and needs no completion aggregate. It may then call the two-argument
`planner.next(cursor, request)` overload without maintaining a synthetic
request sequence. The session-checked planner overload and
`CompletionAggregate` require a valid non-zero work identity and per-request
sequence.

## Forward planning

`ForwardPlanner::begin()` checks the complete range before activating a
caller-owned cursor. It returns no partial plan for a gap, overflow, rejected
work, invalid quantity limits or policies, or a forbidden cross-endpoint
ordered write. Unknown enum values fail closed.

```cpp
#include <modbus_rtu_bridge/ForwardPlan.h>

ForwardPlanner planner(
    RouteTableView(routes, 2),
    ForwardPlanOptions(64, 32));  // coil and holding-register maxima
HoldingForwardCursor cursor;

if (planner.begin(work, session, cursor) == ForwardPlanStatus::Ready) {
  uint32_t sequence = 0;
  while (cursor.active) {
    sequence = nextNonZeroSequence32(sequence);
    PlannedWriteRequest request;
    if (planner.next(cursor, sequence, request) !=
        ForwardNextStatus::Planned) {
      break;
    }
    // request.sourceOffset points into snapshot; request.quantity never
    // crosses an endpoint or configured Modbus quantity boundary.
  }
}
```

The default policy keeps `Tracked` and `SilentOrdered` work within one
endpoint. `LatestState` work may split across adjacent endpoints, but only
after complete-range preflight. Both policies are configurable in
`ForwardPlanOptions`.

`PlannedWriteRequest` borrows a const slice of the admitted snapshot. Retain
the request beside its queue entry so its exact identity can be checked at
completion. Planning performs no payload copy. Route storage must have passed
`validate()` and remain unchanged for the complete plan lifetime.

## Completion handling

`CompletionAggregate` prevents a multi-fragment success from being announced
after only its final fragment, or a later completion from producing a second
notice after an earlier failure. It expects completions in planner order; a
concurrent runner must reorder them first or supply its own richer aggregate.

```cpp
#include <modbus_rtu_bridge/CompletionTransition.h>

CompletionAggregate aggregate(work.identity, work.delivery);

// planned is the exact PlannedWriteRequest retained with this queue entry.
// registerCache is a caller-owned DesiredAppliedCache<uint16_t>.
const CompletionRecord observed(planned.identity,
                                DownstreamOutcome::Applied);
const CompletionDecision decision =
    resolveCompletion(planned, observed, session, aggregate);

if (decision.current() &&
    applyAppliedImageTransition(registerCache, planned, decision)) {
  if (decision.notice == CompletionNotice::WorkSucceeded) {
    // Publish the one caller-visible success for the complete work.
  }
}
```

Treat a `false` apply result as an integration/configuration fault. The
decision remains caller-owned, so the same transition can be retried under the
correct cache lock without resolving the completion a second time.

Pass a `DownstreamOutcome` only after the runner has finished any retry policy.
`Applied` records that request's immutable snapshot in the applied image. The
three failure outcomes set `DesiredImageDisposition::CallerPolicyRequired` and
never alter the visible image. This is intentional: generic code cannot safely
restore a failed range without proving that a newer accepted generation does
not own some or all of that range.

The caller selects retry, readback, range rollback, endpoint rollback, or no
change. If it selects rollback, it must prove current ownership under the same
lock as the visible cache. A first failure settles the public result. If later
fragments were already dispatched, continue resolving them in order so real
downstream applications still reach the applied image; they cannot emit a
success or second failure notice. If the runner can prove none remain in
flight, it may discard the aggregate. Rejected identities, stale sessions,
out-of-order fragments, duplicates, and completions after the final fragment
return a non-current status and cannot mutate the applied image or emit a
notice.

The aggregate orders fragments within one work item; it does not serialize two
overlapping work items. The caller must preserve their downstream order,
coalesce them before dispatch, or reject an older completion using its own
range/endpoint watermark. `workIdentityBefore()` provides wrap-safe token
comparison within one session for that policy.

## Downstream execution

`DownstreamRequest` carries standard Modbus operations without depending on a
specific master implementation. `executeDownstreamRequest()` validates the
same quantity/buffer preconditions, performs exactly one backend call and
returns the request sequence, backend result and exception code together.

| Operation | Function code | Required request fields |
| --- | ---: | --- |
| `ReadCoils` | 1 | endpoint, start, non-zero quantity, `coilBuffer` |
| `ReadDiscreteInputs` | 2 | endpoint, start, non-zero quantity, `coilBuffer` |
| `ReadHoldingRegisters` | 3 | endpoint, start, non-zero quantity, `registerBuffer` |
| `ReadInputRegisters` | 4 | endpoint, start, non-zero quantity, `registerBuffer` |
| `WriteSingleCoil` | 5 | endpoint, start, `coilValue` |
| `WriteSingleHoldingRegister` | 6 | endpoint, start, `registerValue` |
| `WriteMultipleCoils` | 15 | endpoint, start, non-zero quantity, `coilBuffer` |
| `WriteMultipleHoldingRegisters` | 16 | endpoint, start, non-zero quantity, `registerBuffer` |

An adapter that must retain an established validator/error-log path may call
`executeValidatedDownstreamRequest()` after that validator succeeds. Its
precondition is explicit: quantity, buffer and operation must already be valid.
This lets an adapter retain its own validation and error classification without
performing the same checks twice.

```cpp
#include <modbus_rtu_bridge/DownstreamExecutor.h>

struct MasterBackend {
  typedef int Result;
  // Implement result factories, exceptionCode(), and the eight operation
  // methods documented above executeDownstreamRequest(). Each method receives
  // the complete request, including an optional consistencyContext.
};

MasterBackend backend;
bool values[8] = {};
ModbusRTUBridge::DownstreamRequest request;
request.sequence = 42;
request.operation = ModbusRTUBridge::DownstreamOperation::ReadCoils;
request.endpointId = 7;
request.startAddress = 12;
request.quantity = 8;
request.coilBuffer = values;

const ModbusRTUBridge::DownstreamCompletion<int> completion =
    ModbusRTUBridge::executeDownstreamRequest(backend, request);
```

The backend's `Result` may be an enum or another cheaply copied product result.
For a valid request the executor calls exactly one matching operation and then
captures `backend.exceptionCode()`. For a locally invalid request it returns
the matching invalid-result factory with exception code zero and performs no
operation call.

`consistencyContext` is never inspected by the core. If a backend casts it to
a mutex or copy-policy type, that object must stay alive and correctly aligned
until the synchronous backend call returns. Keep application-specific types in
the backend rather than exposing them through a reusable API wrapper.

Queueing, retry counts, pacing, deadlines, worker priorities, and nonstandard
operations belong to the runner. They are not silently treated as standard
Modbus operations here.

The executor neither serializes an ADU nor defines Modbus quantity maxima. A
product adapter that already applies stricter master-specific limits should
retain that validator and use `executeValidatedDownstreamRequest()` only after
it succeeds. This preserves the established error/log path and avoids a second
validation pass; calling the validated form with bad data is a caller bug.
