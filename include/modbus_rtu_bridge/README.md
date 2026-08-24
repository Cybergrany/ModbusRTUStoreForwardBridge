# Public API guide

All APIs in this directory are allocation-free, C++11-compatible and
platform-neutral. They operate on caller-owned storage and perform no serial,
thread, clock or logging operations.

## Route tables

Build one `EndpointRoute` per downstream unit. Ranges are half-open and the
route array must be ordered by upstream start for every populated table.

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

// After a later strict write fails locally/downstream:
coils.restore(8, 3); // visible image returns to last applied state
```

The caller must retain the same lock scope as before migration. Copy methods do
not lock and source/destination ranges must not overlap.

## Downstream execution

`DownstreamRequest` carries standard Modbus operations without depending on a
specific master implementation. `executeDownstreamRequest()` validates the
same quantity/buffer preconditions, performs exactly one backend call and
returns the request sequence, backend result and exception code together.

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

Queueing, retry counts, pacing, deadlines and worker priorities belong to the
runner. Product-specific functions such as OGM FC69 latest-state writes remain
in the product adapter and are not silently treated as standard Modbus here.

