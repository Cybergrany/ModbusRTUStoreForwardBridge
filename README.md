# ModbusRTUStoreForwardBridge

`ModbusRTUStoreForwardBridge` is a header-only C++11 core for asynchronous,
cached Modbus gateways. It translates ranges from one local register image to
downstream endpoints, plans deterministic endpoint-local writes, tracks desired
and applied values, and can coordinate bounded work and polling through a
caller-driven facade.

It performs no serial I/O and is not a transparent Modbus proxy. The
application supplies storage arrays, time values, execution capacity and a
backend. Threads, serial pacing, retries, logging and rollback policy remain in
the application.

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
- **Bounded storage and completion debt.** `FixedRingQueue<T>` reuses a
  caller-supplied array. `CompletionLedger` tracks several logical writes,
  validates the exact admitted request, and keeps failure certainty explicit.
  See [`FixedStorage.h`](include/modbus_rtu_bridge/FixedStorage.h),
  [`CompletionLedger.h`](include/modbus_rtu_bridge/CompletionLedger.h), and
  [bounded storage and ledger usage](include/modbus_rtu_bridge/README.md#bounded-storage-and-completion-ledger).
- **Configurable poll fairness.** `PollPlanner` selects due endpoints in
  round-robin order and limits forward-write bursts. Its two-phase API advances
  persistent state only when a phase is consumed. An index-only scan supports
  low-RAM hot paths. See
  [`PollPlanner.h`](include/modbus_rtu_bridge/PollPlanner.h) and
  [poll planning](include/modbus_rtu_bridge/README.md#poll-planning-and-fairness).
- **Cooperative bridge facade.** `StoreForwardBridge` keeps coil and holding
  writes in one FIFO/sequence domain, arbitrates them with polls using the
  configured forward-burst limit, verifies previewed actions at admission and
  rejects stale completions. See
  [`StoreForwardBridge.h`](include/modbus_rtu_bridge/StoreForwardBridge.h) and
  [the facade guide](include/modbus_rtu_bridge/README.md#cooperative-storeforwardbridge).
- **Optional master adapter.** A thin template maps `DownstreamRequest` to the
  common `ModbusRTUMaster` API without making that library a dependency. See
  [`ModbusRTUMasterBackend.h`](include/modbus_rtu_bridge/adapters/ModbusRTUMasterBackend.h)
  and [adapter usage](include/modbus_rtu_bridge/README.md#optional-modbusrtumaster-adapter).
- **Explicit ownership.** Routes, images, payload snapshots, work slots and
  synchronization remain caller-owned. There are no threads, hardware clocks,
  serial ports, virtual calls or allocator calls hidden in the core. Generic
  queue assignment still follows the stored type's own behavior. See
  [the ownership table](include/modbus_rtu_bridge/README.md#ownership-and-concurrency).
- **Bounded regression tests.** Native tests cover routing, gaps, overflow,
  cache copies, mixed-table dispatch order, stale/mutated actions, fairness,
  allocation count, storage size, and paired performance budgets. See
  [Testing](#testing).

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

## Coordinate work cooperatively

The facade uses fixed caller-owned slots and never runs a worker itself:

```cpp
#include <modbus_rtu_bridge/StoreForwardBridge.h>

using namespace ModbusRTUBridge;

StoreForwardWorkSlot workSlots[4];
CompletionLedgerSlot ledgerSlots[4];
CompletionLedger ledger(ledgerSlots, 4);
StoreForwardBridge bridge(
    ForwardPlanner(RouteTableView(routes, 2), ForwardPlanOptions(64, 32)),
    workSlots, 4, ledger,
    PollPlanner(PollPlannerOptions(2)));

const SessionStateView session(true, 1, 1, SessionPhase::Ready);
bridge.admitWork(work, session);  // work may be coil or holding-register data

StoreForwardAction action;
if (bridge.nextAction(session, nowTicks, polls, pollCount, action) ==
    StoreForwardNextStatus::Ready) {
  // Call admitAction only after reserving transport/worker capacity. Pass the
  // same session and poll candidates used for the preview.
  const StoreForwardActionAdmitStatus admitted =
      bridge.admitAction(action, session, polls, pollCount);
  if (admitted == StoreForwardActionAdmitStatus::Admitted) {
    // Execute action.downstream. Retry outside the library, then report one
    // terminal outcome while keeping action and its payload immutable.
    const StoreForwardCompletion result =
        bridge.complete(action, DownstreamOutcome::Applied, session);
  }
}
```

`nextAction()` is a side-effect-free preview. Queue rejection therefore does
not move the work or poll cursor. `admitAction()` rechecks the proposal and is
the state-changing publication point. A terminal failure stops unissued
fragments, reports whether the send was definite or uncertain, and never rolls
the desired image back automatically.

Payload snapshots remain caller-owned and immutable until the returned
completion reports `workRetired`. Idle stale or cancelled work can be removed
one item at a time with `discardOneStaleWork()` or `abandonFront()`; both return
its identity and `userContext` so the caller can reclaim that snapshot. The
bulk `reset()` is intended only for storage whose lifetime is reclaimed as one
pool. See the [facade lifetime example](include/modbus_rtu_bridge/README.md#cooperative-storeforwardbridge).

## Store-and-forward flow

A typical integration is:

1. Validate an upstream write and reserve space for its immutable snapshot and
   bounded work/ledger slot.
2. Update the visible cache and publish work in the application's chosen
   order.
3. Create an immutable ingress view and either use `ForwardPlanner` directly or
   admit it to `StoreForwardBridge`. Complete-range routing is proved before
   any fragment is exposed.
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
traces, full-range atomicity, wraparound identities, stale and mutated actions,
cross-table FIFO order, poll starvation/decline paths, exactly-once notices,
zero allocations, bounded object sizes, first/worst-slot ledger lookup, and
paired performance lanes. Serial timing and electrical bus behavior belong to
the application that integrates it.

## License

MIT. See [LICENSE](LICENSE).
