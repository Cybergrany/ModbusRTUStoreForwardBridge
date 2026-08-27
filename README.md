# ModbusRTUStoreForwardBridge

`ModbusRTUStoreForwardBridge` is a header-only C++11 core for asynchronous,
cached Modbus gateways. It translates ranges from one local register image to
downstream endpoints, plans deterministic endpoint-local writes, tracks desired
and applied values, and dispatches typed requests through a caller-provided
backend.

It performs no serial I/O and is not a transparent Modbus proxy. The
application owns request admission, storage, queues, scheduling, retries,
logging, and failure/rollback policy.

## When to use it

Use this library when:

- an upstream Modbus server must respond from a fast local cache;
- downstream devices may be slower or temporarily unavailable;
- one upstream range spans several downstream unit IDs or address ranges;
- accepted writes must keep an immutable snapshot until a worker processes
  them; or
- the gateway must use fixed caller-owned storage with no dynamic allocation.

If every upstream request must synchronously proxy one downstream request,
this is probably not the right abstraction.

## What this fork adds

- **Validated range routing.** `RouteTableView` checks sorted, non-overlapping
  mappings and proves that a complete request is routable before returning any
  segment. This prevents a cross-device write from being partly dispatched
  before an unmapped gap is found. See
  [`RouteTable.h`](include/modbus_rtu_bridge/RouteTable.h) and the
  [route-table guide](include/modbus_rtu_bridge/README.md#route-tables).
- **Desired/applied cache pairs.** `DesiredAppliedCache<T>` keeps the visible
  local image separate from the last downstream-applied image. This supports
  optimistic local writes, success commits, and caller-directed rollback. See
  [`CacheImage.h`](include/modbus_rtu_bridge/CacheImage.h) and
  [cached images](include/modbus_rtu_bridge/README.md#cached-images).
- **Typed downstream execution.** `DownstreamRequest` covers standard coil and
  register operations. The executor validates the request, makes exactly one
  backend call, and returns the sequence, result, and exception together. See
  [`DownstreamExecutor.h`](include/modbus_rtu_bridge/DownstreamExecutor.h) and
  [downstream execution](include/modbus_rtu_bridge/README.md#downstream-execution).
- **Explicit ingress and session identity.** Immutable work views distinguish
  tracked, silent-ordered, latest-state, and rejected writes. Non-zero wrapping
  tokens and session generations prevent stale work from being mistaken for a
  current completion. See
  [`IngressWork.h`](include/modbus_rtu_bridge/IngressWork.h) and
  [ingress work](include/modbus_rtu_bridge/README.md#ingress-work-and-sessions).
- **Atomic forward planning.** `ForwardPlanner` proves the whole range before
  emitting anything, then deterministically splits it at endpoint and quantity
  boundaries without copying the snapshot. See
  [`ForwardPlan.h`](include/modbus_rtu_bridge/ForwardPlan.h) and
  [forward planning](include/modbus_rtu_bridge/README.md#forward-planning).
- **Neutral completion transitions.** Adapters classify downstream results as
  applied, definitely-not-sent, send-uncertain, or failed-after-send. The core
  aggregates ordered fragments, rejects stale or duplicate completions, emits
  at most one work notice, and advances only successfully applied snapshots.
  Failures never roll visible state back without caller-owned generation
  proof. See
  [`CompletionTransition.h`](include/modbus_rtu_bridge/CompletionTransition.h)
  and [completion handling](include/modbus_rtu_bridge/README.md#completion-handling).
- **Explicit ownership.** All routes, images, request buffers, cursors, queue
  entries, and synchronization remain caller-owned. There are no threads,
  clocks, serial ports, virtual calls, or allocations hidden in the core. See
  [the ownership table](include/modbus_rtu_bridge/README.md#ownership-and-concurrency).
- **Bounded regression tests.** Native tests cover routing, gaps, overflow,
  cache copies, validation, dispatch order, and paired route-lookup and
  forward-planning performance budgets. See [Testing](#testing).

## Quick start: route a flattened range

```cpp
#include <modbus_rtu_bridge/RouteTable.h>

using namespace ModbusRTUBridge;

constexpr uint8_t kHolding =
    static_cast<uint8_t>(RegisterTable::HoldingRegisters);

EndpointRoute routes[2];

void configureRoutes() {
  routes[0].endpointId = 4;
  routes[0].upstream[kHolding] = AddressRange(100, 4);
  routes[0].downstream[kHolding] = AddressRange(0, 4);

  routes[1].endpointId = 9;
  routes[1].upstream[kHolding] = AddressRange(104, 3);
  routes[1].downstream[kHolding] = AddressRange(20, 3);
}

bool dispatchRange(uint16_t start, uint16_t count) {
  RouteTableView table(routes, 2);
  if (table.validate() != RouteTableStatus::Valid) {
    return false;
  }

  RouteCursor cursor;
  if (table.beginSpan(RegisterTable::HoldingRegisters,
                      start, count, cursor) != RoutedSpanStatus::Complete) {
    return false;
  }

  RouteSegment segment;
  while (table.next(cursor, segment)) {
    // Queue segment.endpointId, segment.downstreamStart, and segment.count.
    // segment.sourceOffset selects values from the immutable input snapshot.
  }
  return true;
}
```

Call `validate()` after building or changing the route table and before
accepting traffic. Keep route storage unchanged while a cursor is active.

## Track visible and applied state

```cpp
#include <modbus_rtu_bridge/CacheImage.h>

bool visible[32] = {};
bool applied[32] = {};

ModbusRTUBridge::DesiredAppliedCache<bool> coils(
    ModbusRTUBridge::MutableImageView<bool>(visible, 32),
    ModbusRTUBridge::MutableImageView<bool>(applied, 32));

const bool snapshot[3] = {true, false, true};
coils.captureDesired(8, snapshot, 3);  // accepted locally
coils.markApplied(8, snapshot, 3);     // downstream success
coils.restore(8, 3);                   // only after caller proves ownership
```

The library does not lock these arrays. The caller must hold the appropriate
lock around each logically atomic cache operation.

## Plan an immutable write

```cpp
#include <modbus_rtu_bridge/ForwardPlan.h>

using namespace ModbusRTUBridge;

const uint16_t values[3] = {10, 20, 30};
const HoldingIngressWorkView work = makeHoldingIngressWork(
    WorkIdentity(7, 1), 102, 3, IngressDelivery::Tracked, values);

// Use only after configureRoutes() and RouteTableView(routes, 2).validate()
// have succeeded.
ForwardPlanner planner(
    RouteTableView(routes, 2),
    ForwardPlanOptions(64, 32));
HoldingForwardCursor cursor;

if (planner.begin(work, cursor) == ForwardPlanStatus::Ready) {
  uint32_t sequence = 0;
  while (cursor.active) {
    sequence = nextNonZeroSequence32(sequence);
    PlannedWriteRequest request;
    if (planner.next(cursor, sequence, request) !=
        ForwardNextStatus::Planned) {
      break;
    }
    // Submit request to the caller-owned downstream runner. The values pointer
    // borrows directly from the immutable array above.
  }
}
```

The default plan keeps ordered writes within one endpoint and permits a fully
preflighted latest-state write to split across adjacent endpoints. Both span
policies and the maximum request quantities are explicit configuration.
The zero `WorkIdentity()` is also valid for planning when an integration owns
ordering elsewhere and does not use session checks or completion aggregation.
Such integrations may use `planner.next(cursor, request)` without inventing a
request sequence.

## Store-and-forward flow

A typical integration is:

1. Validate an upstream write and reserve space for its immutable snapshot.
2. Update the visible cache and publish work in the application's chosen
   order.
3. Create an immutable ingress view and use `ForwardPlanner` to prove complete
   route coverage before producing endpoint-local, quantity-bounded requests.
4. Run typed downstream requests from a caller-owned worker or cooperative
   loop.
5. Resolve terminal downstream results in planner order. The completion
   aggregate emits at most one notice and can mark successful snapshots as
   applied under the caller's lock.
6. On failure, use caller-owned retry/readback/rollback policy. The generic
   layer deliberately leaves visible desired data unchanged because it cannot
   prove that a newer work generation does not own the range.

The detailed backend contract and examples are in the
[public API guide](include/modbus_rtu_bridge/README.md).

## Testing

Run the standalone C++11 behavior, route-lookup, and forward-planning checks:

```sh
scripts/run_native_tests.sh
```

These tests cover the platform-neutral core, including deterministic planning
traces, full-range atomicity, wraparound identities, stale and out-of-order
completions, exactly-once notices, and superseded desired data. Serial timing,
queue scheduling, and electrical bus behavior belong to the application that
integrates it.

## License

MIT. See [LICENSE](LICENSE).
