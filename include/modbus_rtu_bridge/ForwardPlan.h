#pragma once

// Deterministic planning of accepted writes into endpoint-local Modbus
// requests.
//
// ForwardPlanner owns no routes, snapshots, buffers, queue, or sequence
// counter. begin() proves that the complete upstream range is routable before
// next() returns any request. next() then borrows directly from the immutable
// ingress snapshot, so planning adds no payload copy.

#include <stdint.h>

#include <modbus_rtu_bridge/DownstreamExecutor.h>
#include <modbus_rtu_bridge/IngressWork.h>
#include <modbus_rtu_bridge/RouteTable.h>

namespace ModbusRTUBridge {

enum class ForwardSpanPolicy : uint8_t {
  // Reject work that crosses an endpoint boundary even when the complete
  // range is otherwise routable.
  SingleEndpoint = 0U,

  // Prove complete route coverage, then emit endpoint-local fragments.
  PreflightAndSplit = 1U,
};

inline bool isKnownForwardSpanPolicy(ForwardSpanPolicy policy) {
  switch(policy){
    case ForwardSpanPolicy::SingleEndpoint:
    case ForwardSpanPolicy::PreflightAndSplit:
      return true;
    default:
      return false;
  }
}

struct ForwardPlanOptions {
  uint16_t maxCoilsPerRequest;
  uint16_t maxHoldingRegistersPerRequest;
  ForwardSpanPolicy orderedSpanPolicy;
  ForwardSpanPolicy latestStateSpanPolicy;

  ForwardPlanOptions()
      : maxCoilsPerRequest(0U),
        maxHoldingRegistersPerRequest(0U),
        orderedSpanPolicy(ForwardSpanPolicy::SingleEndpoint),
        latestStateSpanPolicy(ForwardSpanPolicy::PreflightAndSplit) {}

  ForwardPlanOptions(uint16_t maxCoils,
                     uint16_t maxHolding,
                     ForwardSpanPolicy orderedPolicy =
                         ForwardSpanPolicy::SingleEndpoint,
                     ForwardSpanPolicy latestPolicy =
                         ForwardSpanPolicy::PreflightAndSplit)
      : maxCoilsPerRequest(maxCoils),
        maxHoldingRegistersPerRequest(maxHolding),
        orderedSpanPolicy(orderedPolicy),
        latestStateSpanPolicy(latestPolicy) {}
};

enum class ForwardPlanStatus : uint8_t {
  Ready = 0U,
  NotAccepted,
  InvalidWork,
  StaleSession,
  InvalidLimits,
  Empty,
  AddressOverflow,
  Gap,
  MultipleEndpointsNotAllowed,
  InvalidPolicy,
};

enum class ForwardNextStatus : uint8_t {
  Planned = 0U,
  Complete,
  InvalidSequence,
  TopologyChanged,
};

inline uint32_t nextNonZeroSequence32(uint32_t value) {
  const uint32_t next = value + 1UL;
  return next == 0UL ? 1UL : next;
}

struct RequestIdentity {
  WorkIdentity work;
  uint32_t requestSequence;
  uint16_t fragmentIndex;

  RequestIdentity()
      : work(), requestSequence(0UL), fragmentIndex(0U) {}

  RequestIdentity(const WorkIdentity& workIdentity,
                  uint32_t sequence,
                  uint16_t fragment)
      : work(workIdentity),
        requestSequence(sequence),
        fragmentIndex(fragment) {}

  bool valid() const {
    return work.valid() && requestSequence != 0UL;
  }
};

inline bool sameRequestIdentity(const RequestIdentity& lhs,
                                const RequestIdentity& rhs) {
  return sameWorkIdentity(lhs.work, rhs.work) &&
         lhs.requestSequence == rhs.requestSequence &&
         lhs.fragmentIndex == rhs.fragmentIndex;
}

// A const-correct write request produced from an immutable snapshot.
//
// The existing DownstreamRequest remains suitable for general read/write
// backends. This write-only plan uses const payload pointers so a planner can
// never expose an admitted snapshot as mutable state.
struct PlannedWriteRequest {
  RequestIdentity identity;
  IngressDelivery delivery;
  RegisterTable table;
  DownstreamOperation operation;
  uint16_t routeIndex;
  uint8_t endpointId;
  uint16_t upstreamStart;
  uint16_t downstreamStart;
  uint16_t quantity;
  uint16_t sourceOffset;
  const bool* coilValues;
  const uint16_t* holdingValues;
  bool coilValue;
  uint16_t holdingValue;
  bool finalFragment;

  PlannedWriteRequest()
      : identity(),
        delivery(IngressDelivery::Rejected),
        table(RegisterTable::Coils),
        operation(DownstreamOperation::WriteSingleCoil),
        routeIndex(0U),
        endpointId(0U),
        upstreamStart(0U),
        downstreamStart(0U),
        quantity(0U),
        sourceOffset(0U),
        coilValues(nullptr),
        holdingValues(nullptr),
        coilValue(false),
        holdingValue(0U),
        finalFragment(false) {}
};

template<typename Value>
struct ForwardCursor {
  IngressWorkView<Value> work;
  uint16_t nextAddress;
  uint16_t remaining;
  uint16_t sourceOffset;
  uint16_t fragmentIndex;
  bool active;

  ForwardCursor()
      : work(),
        nextAddress(0U),
        remaining(0U),
        sourceOffset(0U),
        fragmentIndex(0U),
        active(false) {}
};

typedef ForwardCursor<bool> CoilForwardCursor;
typedef ForwardCursor<uint16_t> HoldingForwardCursor;

namespace ForwardPlanDetail {

template<typename Value>
struct WorkTraits;

template<>
struct WorkTraits<bool> {
  static RegisterTable table() { return RegisterTable::Coils; }

  static uint16_t maxQuantity(const ForwardPlanOptions& options) {
    return options.maxCoilsPerRequest;
  }

  static void populatePayload(const bool* values,
                              uint16_t quantity,
                              PlannedWriteRequest& request) {
    request.coilValues = values;
    request.coilValue = values[0];
    request.operation = quantity == 1U
        ? DownstreamOperation::WriteSingleCoil
        : DownstreamOperation::WriteMultipleCoils;
  }
};

template<>
struct WorkTraits<uint16_t> {
  static RegisterTable table() { return RegisterTable::HoldingRegisters; }

  static uint16_t maxQuantity(const ForwardPlanOptions& options) {
    return options.maxHoldingRegistersPerRequest;
  }

  static void populatePayload(const uint16_t* values,
                              uint16_t quantity,
                              PlannedWriteRequest& request) {
    request.holdingValues = values;
    request.holdingValue = values[0];
    request.operation = quantity == 1U
        ? DownstreamOperation::WriteSingleHoldingRegister
        : DownstreamOperation::WriteMultipleHoldingRegisters;
  }
};

inline ForwardPlanStatus mapSpanStatus(RoutedSpanStatus status) {
  switch(status){
    case RoutedSpanStatus::Complete:
      return ForwardPlanStatus::Ready;
    case RoutedSpanStatus::Empty:
      return ForwardPlanStatus::Empty;
    case RoutedSpanStatus::AddressOverflow:
      return ForwardPlanStatus::AddressOverflow;
    case RoutedSpanStatus::Gap:
    default:
      return ForwardPlanStatus::Gap;
  }
}

}  // namespace ForwardPlanDetail

class ForwardPlanner {
 public:
  ForwardPlanner() : routes_(), options_() {}

  ForwardPlanner(RouteTableView routes, const ForwardPlanOptions& options)
      : routes_(routes), options_(options) {}

  const RouteTableView& routes() const { return routes_; }
  const ForwardPlanOptions& options() const { return options_; }

  template<typename Value>
  ForwardPlanStatus begin(const IngressWorkView<Value>& work,
                          ForwardCursor<Value>& cursor) const {
    cursor = ForwardCursor<Value>();
    if(!isAccepted(work.delivery)){
      return ForwardPlanStatus::NotAccepted;
    }
    if(work.count == 0U){
      return ForwardPlanStatus::Empty;
    }
    const uint32_t end =
        static_cast<uint32_t>(work.start) + static_cast<uint32_t>(work.count);
    if(end > 0x10000UL){
      return ForwardPlanStatus::AddressOverflow;
    }
    if(!work.snapshot.validFor(work.count)){
      return ForwardPlanStatus::InvalidWork;
    }

    const uint16_t maxQuantity =
        ForwardPlanDetail::WorkTraits<Value>::maxQuantity(options_);
    if(maxQuantity == 0U){
      return ForwardPlanStatus::InvalidLimits;
    }

    const RegisterTable table = ForwardPlanDetail::WorkTraits<Value>::table();
    const RoutedSpanStatus span = routes_.inspectSpan(
        table, work.start, work.count);
    if(span != RoutedSpanStatus::Complete){
      return ForwardPlanDetail::mapSpanStatus(span);
    }

    const ForwardSpanPolicy spanPolicy = isLatestState(work.delivery)
        ? options_.latestStateSpanPolicy
        : options_.orderedSpanPolicy;
    switch(spanPolicy){
      case ForwardSpanPolicy::SingleEndpoint: {
        uint16_t routeIndex = 0U;
        if(!routes_.locate(table, work.start, routeIndex)){
          return ForwardPlanStatus::Gap;
        }
        const AddressRange& endpointRange =
            routes_.data()[routeIndex].upstream[static_cast<uint8_t>(table)];
        const uint32_t workEnd = static_cast<uint32_t>(work.start) +
                                 static_cast<uint32_t>(work.count);
        if(workEnd > endpointRange.end()){
          return ForwardPlanStatus::MultipleEndpointsNotAllowed;
        }
        break;
      }
      case ForwardSpanPolicy::PreflightAndSplit:
        break;
      default:
        return ForwardPlanStatus::InvalidPolicy;
    }

    cursor.work = work;
    cursor.nextAddress = work.start;
    cursor.remaining = work.count;
    cursor.sourceOffset = 0U;
    cursor.fragmentIndex = 0U;
    cursor.active = true;
    return ForwardPlanStatus::Ready;
  }

  template<typename Value>
  ForwardPlanStatus begin(const IngressWorkView<Value>& work,
                          const SessionStateView& session,
                          ForwardCursor<Value>& cursor) const {
    // Unlike the basic overload, this path deliberately requires a non-zero
    // WorkIdentity because workCurrent() ties the plan to one ready session.
    if(!workCurrent(work.identity, session)){
      cursor = ForwardCursor<Value>();
      return ForwardPlanStatus::StaleSession;
    }
    return begin(work, cursor);
  }

  template<typename Value>
  ForwardNextStatus next(ForwardCursor<Value>& cursor,
                         uint32_t requestSequence,
                         PlannedWriteRequest& request) const {
    request = PlannedWriteRequest();
    if(!cursor.active || cursor.remaining == 0U){
      cursor.active = false;
      return ForwardNextStatus::Complete;
    }
    // Zero is valid only for the deliberately identityless planning path.
    // A plan that participates in completion aggregation needs an exact,
    // non-zero per-request sequence in addition to its WorkIdentity.
    if(requestSequence == 0UL && cursor.work.identity.valid()){
      return ForwardNextStatus::InvalidSequence;
    }

    const RegisterTable table = ForwardPlanDetail::WorkTraits<Value>::table();
    uint16_t routeIndex = 0U;
    if(!routes_.locate(table, cursor.nextAddress, routeIndex)){
      cursor.active = false;
      cursor.remaining = 0U;
      return ForwardNextStatus::TopologyChanged;
    }

    const EndpointRoute& route = routes_.data()[routeIndex];
    const uint8_t tableIndex = static_cast<uint8_t>(table);
    const AddressRange& upstream = route.upstream[tableIndex];
    const AddressRange& downstream = route.downstream[tableIndex];
    const uint16_t routeOffset =
        static_cast<uint16_t>(cursor.nextAddress - upstream.start);
    const uint16_t routeAvailable =
        static_cast<uint16_t>(upstream.count - routeOffset);
    const uint16_t maxQuantity =
        ForwardPlanDetail::WorkTraits<Value>::maxQuantity(options_);
    uint16_t quantity = cursor.remaining < routeAvailable
        ? cursor.remaining
        : routeAvailable;
    if(quantity > maxQuantity){
      quantity = maxQuantity;
    }
    if(quantity == 0U){
      cursor.active = false;
      cursor.remaining = 0U;
      return ForwardNextStatus::TopologyChanged;
    }

    request.identity = RequestIdentity(
        cursor.work.identity, requestSequence, cursor.fragmentIndex);
    request.delivery = cursor.work.delivery;
    request.table = table;
    request.routeIndex = routeIndex;
    request.endpointId = route.endpointId;
    request.upstreamStart = cursor.nextAddress;
    request.downstreamStart = static_cast<uint16_t>(
        downstream.start + routeOffset);
    request.quantity = quantity;
    request.sourceOffset = cursor.sourceOffset;
    request.finalFragment = cursor.remaining == quantity;
    ForwardPlanDetail::WorkTraits<Value>::populatePayload(
        cursor.work.snapshot.data() + cursor.sourceOffset,
        quantity,
        request);

    cursor.nextAddress = static_cast<uint16_t>(cursor.nextAddress + quantity);
    cursor.remaining = static_cast<uint16_t>(cursor.remaining - quantity);
    cursor.sourceOffset = static_cast<uint16_t>(cursor.sourceOffset + quantity);
    cursor.fragmentIndex = static_cast<uint16_t>(cursor.fragmentIndex + 1U);
    if(cursor.remaining == 0U){
      cursor.active = false;
    }
    return ForwardNextStatus::Planned;
  }

  // Low-boilerplate overload for cooperative integrations that already own
  // ordering and used WorkIdentity(). The resulting RequestIdentity is
  // intentionally invalid and cannot enter CompletionAggregate.
  template<typename Value>
  ForwardNextStatus next(ForwardCursor<Value>& cursor,
                         PlannedWriteRequest& request) const {
    return next(cursor, 0UL, request);
  }

 private:
  RouteTableView routes_;
  ForwardPlanOptions options_;
};

}  // namespace ModbusRTUBridge
