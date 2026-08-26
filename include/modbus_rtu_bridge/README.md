# Public API guide

All APIs in this directory are allocation-free, C++11-compatible and
platform-neutral. They operate on caller-owned storage and perform no serial,
thread, clock or logging operations.

## Ownership and concurrency

| Contract | Storage owner | Hidden state/allocation | Synchronization |
| --- | --- | --- | --- |
| `RouteTableView` | Caller owns immutable `EndpointRoute[]`. | None. A `RouteCursor` is caller-owned iteration state. | Caller prevents route mutation while any lookup/cursor may run. |
| `MutableImageView<T>` | Caller owns the visible/applied arrays. | None. The view is only a pointer and count. | Caller retains the existing cache lock around each logically atomic operation. |
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

// After a later strict write fails locally/downstream:
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
5. On a product-defined rollback path, call `restore()` for exactly the failed
   range. The core does not decide whether a result is terminal or retryable.

All copy helpers fail closed on a null pointer, zero count or out-of-range
span. They return `false`; they do not truncate a request or partially copy it.

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
