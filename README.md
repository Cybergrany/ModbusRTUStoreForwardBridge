# ModbusRTUStoreForwardBridge

`ModbusRTUStoreForwardBridge` is a header-only C++11 core for asynchronous,
cached Modbus gateways. It translates ranges from one local register image to
downstream endpoints, tracks desired and applied values, and dispatches typed
requests through a caller-provided backend.

It performs no serial I/O and is not a transparent Modbus proxy. The
application owns request admission, storage, queues, scheduling, retries,
logging, and completion policy.

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
- **Explicit ownership.** All routes, images, request buffers, cursors, queue
  entries, and synchronization remain caller-owned. There are no threads,
  clocks, serial ports, virtual calls, or allocations hidden in the core. See
  [the ownership table](include/modbus_rtu_bridge/README.md#ownership-and-concurrency).
- **Bounded compatibility tests.** Native tests cover routing, gaps, overflow,
  cache copies, validation, dispatch order, and a paired route-lookup
  performance budget. See [Testing](#testing).

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
coils.restore(8, 3);                   // caller-selected rollback
```

The library does not lock these arrays. The caller must hold the appropriate
lock around each logically atomic cache operation.

## Store-and-forward flow

A typical integration is:

1. Validate an upstream write and reserve space for its immutable snapshot.
2. Update the visible cache and publish work in the application's chosen
   order.
3. Prove complete route coverage, then split the snapshot into endpoint-local
   segments.
4. Run typed downstream requests from a caller-owned worker or cooperative
   loop.
5. Mark values applied, retry, or restore them according to application
   policy.

The detailed backend contract and examples are in the
[public API guide](include/modbus_rtu_bridge/README.md).

## Testing

Run the standalone C++11 behavior and route-lookup performance checks:

```sh
scripts/run_native_tests.sh
```

These tests cover the platform-neutral core. Serial timing, queue scheduling,
and electrical bus behavior belong to the application that integrates it.

## License

MIT. See [LICENSE](LICENSE).
