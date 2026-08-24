#pragma once

// Platform-neutral route/range translation for a flattened Modbus image.
//
// This file intentionally depends only on fixed-width integer types. It does
// not allocate, own endpoints, perform I/O, or know how a product represents a
// child. The product adapter builds EndpointRoute records from its topology and
// retains ownership of that storage for the lifetime of RouteTableView.

#include <stdint.h>

namespace ModbusRTUBridge {

enum class RegisterTable : uint8_t {
  Coils = 0U,
  DiscreteInputs = 1U,
  HoldingRegisters = 2U,
  InputRegisters = 3U,
  Count = 4U,
};

// A half-open Modbus address range [start, start + count).
//
// A zero count means that the endpoint has no mapping for this table. The
// 32-bit end calculation prevents a 16-bit wrap from turning an invalid route
// into an apparently small valid range.
struct AddressRange {
  uint16_t start;
  uint16_t count;

  AddressRange() : start(0U), count(0U) {}
  AddressRange(uint16_t rangeStart, uint16_t rangeCount)
      : start(rangeStart), count(rangeCount) {}

  uint32_t end() const {
    return static_cast<uint32_t>(start) + static_cast<uint32_t>(count);
  }

  bool contains(uint16_t address) const {
    return count != 0U &&
           static_cast<uint32_t>(address) >= static_cast<uint32_t>(start) &&
           static_cast<uint32_t>(address) < end();
  }
};

// One downstream endpoint's mapping into the flattened upstream image.
//
// The ranges at the same table index describe the same payload. For example,
// upstream[HoldingRegisters] = {120, 8} and downstream[...] = {0, 8}
// translates upstream holding register 123 to endpoint register 3.
struct EndpointRoute {
  uint8_t endpointId;
  AddressRange upstream[static_cast<uint8_t>(RegisterTable::Count)];
  AddressRange downstream[static_cast<uint8_t>(RegisterTable::Count)];

  EndpointRoute() : endpointId(0U), upstream(), downstream() {}
};

enum class RouteTableStatus : uint8_t {
  Valid = 0U,
  NullStorage,
  RangeOverflow,
  CountMismatch,
  Unsorted,
  Overlap,
};

enum class RoutedSpanStatus : uint8_t {
  Complete = 0U,
  Empty,
  AddressOverflow,
  Gap,
};

// A translated, contiguous portion of an upstream request.
// sourceOffset is relative to the beginning of the caller's original request
// and therefore also indexes an immutable ingress snapshot.
struct RouteSegment {
  uint16_t routeIndex;
  uint8_t endpointId;
  uint16_t upstreamStart;
  uint16_t downstreamStart;
  uint16_t count;
  uint16_t sourceOffset;

  RouteSegment()
      : routeIndex(0U),
        endpointId(0U),
        upstreamStart(0U),
        downstreamStart(0U),
        count(0U),
        sourceOffset(0U) {}
};

// Iteration state is caller-owned so one RouteTableView may be used by
// cooperative or threaded adapters without hidden mutable state.
struct RouteCursor {
  RegisterTable table;
  uint16_t nextAddress;
  uint16_t remaining;
  uint16_t sourceOffset;

  RouteCursor()
      : table(RegisterTable::Coils),
        nextAddress(0U),
        remaining(0U),
        sourceOffset(0U) {}
};

class RouteTableView {
 public:
  RouteTableView() : routes_(nullptr), routeCount_(0U) {}

  RouteTableView(const EndpointRoute* routes, uint16_t routeCount)
      : routes_(routes), routeCount_(routeCount) {}

  const EndpointRoute* data() const { return routes_; }
  uint16_t size() const { return routeCount_; }

  // Validate every table independently. Non-empty upstream ranges must be in
  // ascending, non-overlapping order. Downstream and upstream counts must
  // match, but downstream address ranges may overlap across endpoints because
  // each endpoint has its own address space.
  RouteTableStatus validate() const {
    if(routeCount_ != 0U && routes_ == nullptr){
      return RouteTableStatus::NullStorage;
    }

    for(uint8_t tableIndex = 0U;
        tableIndex < static_cast<uint8_t>(RegisterTable::Count);
        ++tableIndex){
      uint32_t previousEnd = 0U;
      bool havePrevious = false;
      for(uint16_t routeIndex = 0U; routeIndex < routeCount_; ++routeIndex){
        const AddressRange& upstream = routes_[routeIndex].upstream[tableIndex];
        const AddressRange& downstream = routes_[routeIndex].downstream[tableIndex];
        if(upstream.count != downstream.count){
          return RouteTableStatus::CountMismatch;
        }
        if(upstream.end() > 0x10000UL || downstream.end() > 0x10000UL){
          return RouteTableStatus::RangeOverflow;
        }
        if(upstream.count == 0U){
          continue;
        }
        if(havePrevious){
          if(static_cast<uint32_t>(upstream.start) < previousEnd){
            return RouteTableStatus::Overlap;
          }
          if(static_cast<uint32_t>(upstream.start) <
             static_cast<uint32_t>(routes_[routeIndex - 1U].upstream[tableIndex].start)){
            return RouteTableStatus::Unsorted;
          }
        }
        previousEnd = upstream.end();
        havePrevious = true;
      }
    }
    return RouteTableStatus::Valid;
  }

  // Locate the endpoint containing one upstream address. Valid populated
  // ranges use the same binary-search shape as the legacy OGM bridge. If a
  // midpoint has no mapping for this table, fall back to the zero-safe linear
  // path; normal populated hot paths retain logarithmic lookup cost.
  bool locate(RegisterTable table,
              uint16_t upstreamAddress,
              uint16_t& routeIndexOut) const {
    if(routes_ == nullptr){
      return false;
    }
    const uint8_t tableIndex = static_cast<uint8_t>(table);
    if(tableIndex >= static_cast<uint8_t>(RegisterTable::Count)){
      return false;
    }
    int16_t low = 0;
    int16_t high = static_cast<int16_t>(routeCount_) - 1;
    while(low <= high){
      const int16_t middle =
          static_cast<int16_t>(low + ((high - low) / 2));
      const AddressRange& range =
          routes_[static_cast<uint16_t>(middle)].upstream[tableIndex];
      if(range.count == 0U){
        return locateWithEmptyMappings(
            tableIndex, upstreamAddress, routeIndexOut);
      }
      if(static_cast<uint32_t>(upstreamAddress) <
         static_cast<uint32_t>(range.start)){
        high = static_cast<int16_t>(middle - 1);
        continue;
      }
      if(static_cast<uint32_t>(upstreamAddress) >= range.end()){
        low = static_cast<int16_t>(middle + 1);
        continue;
      }
      routeIndexOut = static_cast<uint16_t>(middle);
      return true;
    }
    return false;
  }

  // Prove that the complete range is routable before a caller emits any child
  // request. This is the guard that prevents a cross-endpoint visual frame
  // containing a gap from being partially applied downstream.
  RoutedSpanStatus inspectSpan(RegisterTable table,
                               uint16_t start,
                               uint16_t count) const {
    if(count == 0U){
      return RoutedSpanStatus::Empty;
    }
    const uint32_t requestedEnd =
        static_cast<uint32_t>(start) + static_cast<uint32_t>(count);
    if(requestedEnd > 0x10000UL){
      return RoutedSpanStatus::AddressOverflow;
    }

    uint16_t current = start;
    uint16_t remaining = count;
    while(remaining != 0U){
      uint16_t routeIndex = 0U;
      if(!locate(table, current, routeIndex)){
        return RoutedSpanStatus::Gap;
      }
      const AddressRange& range =
          routes_[routeIndex].upstream[static_cast<uint8_t>(table)];
      const uint16_t available = static_cast<uint16_t>(range.end() - current);
      const uint16_t chunk = remaining < available ? remaining : available;
      current = static_cast<uint16_t>(current + chunk);
      remaining = static_cast<uint16_t>(remaining - chunk);
    }
    return RoutedSpanStatus::Complete;
  }

  RoutedSpanStatus beginSpan(RegisterTable table,
                             uint16_t start,
                             uint16_t count,
                             RouteCursor& cursor) const {
    const RoutedSpanStatus status = inspectSpan(table, start, count);
    if(status != RoutedSpanStatus::Complete){
      cursor = RouteCursor();
      return status;
    }
    cursor.table = table;
    cursor.nextAddress = start;
    cursor.remaining = count;
    cursor.sourceOffset = 0U;
    return RoutedSpanStatus::Complete;
  }

  // Return the next endpoint-local segment. Call only after beginSpan()
  // returned Complete. false means normal end-of-range, never a hidden gap.
  bool next(RouteCursor& cursor, RouteSegment& segmentOut) const {
    if(cursor.remaining == 0U){
      return false;
    }
    uint16_t routeIndex = 0U;
    if(!locate(cursor.table, cursor.nextAddress, routeIndex)){
      // Defensive fail-closed behavior if the route storage was mutated after
      // beginSpan(). Valid immutable route tables never take this branch.
      cursor.remaining = 0U;
      return false;
    }

    const uint8_t tableIndex = static_cast<uint8_t>(cursor.table);
    const EndpointRoute& route = routes_[routeIndex];
    const AddressRange& upstream = route.upstream[tableIndex];
    const AddressRange& downstream = route.downstream[tableIndex];
    const uint16_t offset = static_cast<uint16_t>(cursor.nextAddress - upstream.start);
    const uint16_t available = static_cast<uint16_t>(upstream.count - offset);
    const uint16_t chunk = cursor.remaining < available ? cursor.remaining : available;

    segmentOut.routeIndex = routeIndex;
    segmentOut.endpointId = route.endpointId;
    segmentOut.upstreamStart = cursor.nextAddress;
    segmentOut.downstreamStart = static_cast<uint16_t>(downstream.start + offset);
    segmentOut.count = chunk;
    segmentOut.sourceOffset = cursor.sourceOffset;

    cursor.nextAddress = static_cast<uint16_t>(cursor.nextAddress + chunk);
    cursor.remaining = static_cast<uint16_t>(cursor.remaining - chunk);
    cursor.sourceOffset = static_cast<uint16_t>(cursor.sourceOffset + chunk);
    return true;
  }

 private:
  bool locateWithEmptyMappings(uint8_t tableIndex,
                               uint16_t upstreamAddress,
                               uint16_t& routeIndexOut) const {
    for(uint16_t routeIndex = 0U; routeIndex < routeCount_; ++routeIndex){
      const AddressRange& range = routes_[routeIndex].upstream[tableIndex];
      if(range.contains(upstreamAddress)){
        routeIndexOut = routeIndex;
        return true;
      }
      if(range.count != 0U && upstreamAddress < range.start){
        break;
      }
    }
    return false;
  }

  const EndpointRoute* routes_;
  uint16_t routeCount_;
};

}  // namespace ModbusRTUBridge
