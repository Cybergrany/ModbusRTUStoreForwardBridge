#include <modbus_rtu_bridge/CacheImage.h>
#include <modbus_rtu_bridge/DownstreamExecutor.h>
#include <modbus_rtu_bridge/RouteTable.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

unsigned g_checks = 0U;

#define CHECK(condition)                                                        \
  do {                                                                          \
    ++g_checks;                                                                 \
    if(!(condition)){                                                           \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,     \
              #condition);                                                      \
      exit(1);                                                                  \
    }                                                                           \
  } while(false)

using namespace ModbusRTUBridge;

EndpointRoute makeHoldingRoute(uint8_t endpoint,
                               uint16_t upstreamStart,
                               uint16_t downstreamStart,
                               uint16_t count) {
  EndpointRoute route;
  route.endpointId = endpoint;
  route.upstream[static_cast<uint8_t>(RegisterTable::HoldingRegisters)] =
      AddressRange(upstreamStart, count);
  route.downstream[static_cast<uint8_t>(RegisterTable::HoldingRegisters)] =
      AddressRange(downstreamStart, count);
  return route;
}

void testRouteValidationAndTranslation() {
  RouteTableView empty(nullptr, 0U);
  CHECK(empty.validate() == RouteTableStatus::Valid);

  RouteTableView missing(nullptr, 1U);
  CHECK(missing.validate() == RouteTableStatus::NullStorage);

  EndpointRoute routes[3];
  routes[0] = makeHoldingRoute(4U, 100U, 0U, 4U);
  routes[1] = makeHoldingRoute(9U, 104U, 20U, 3U);
  routes[2] = makeHoldingRoute(12U, 110U, 7U, 2U);
  RouteTableView table(routes, 3U);
  CHECK(table.validate() == RouteTableStatus::Valid);

  uint16_t routeIndex = 99U;
  CHECK(table.locate(RegisterTable::HoldingRegisters, 100U, routeIndex));
  CHECK(routeIndex == 0U);
  CHECK(table.locate(RegisterTable::HoldingRegisters, 106U, routeIndex));
  CHECK(routeIndex == 1U);
  CHECK(!table.locate(RegisterTable::HoldingRegisters, 107U, routeIndex));

  CHECK(table.inspectSpan(RegisterTable::HoldingRegisters, 102U, 5U) ==
        RoutedSpanStatus::Complete);
  CHECK(table.inspectSpan(RegisterTable::HoldingRegisters, 102U, 6U) ==
        RoutedSpanStatus::Gap);
  CHECK(table.inspectSpan(RegisterTable::HoldingRegisters, 0U, 0U) ==
        RoutedSpanStatus::Empty);
  CHECK(table.inspectSpan(RegisterTable::HoldingRegisters, 65535U, 2U) ==
        RoutedSpanStatus::AddressOverflow);

  RouteCursor cursor;
  CHECK(table.beginSpan(RegisterTable::HoldingRegisters, 102U, 5U, cursor) ==
        RoutedSpanStatus::Complete);
  RouteSegment segment;
  CHECK(table.next(cursor, segment));
  CHECK(segment.routeIndex == 0U);
  CHECK(segment.endpointId == 4U);
  CHECK(segment.upstreamStart == 102U);
  CHECK(segment.downstreamStart == 2U);
  CHECK(segment.count == 2U);
  CHECK(segment.sourceOffset == 0U);
  CHECK(table.next(cursor, segment));
  CHECK(segment.routeIndex == 1U);
  CHECK(segment.endpointId == 9U);
  CHECK(segment.upstreamStart == 104U);
  CHECK(segment.downstreamStart == 20U);
  CHECK(segment.count == 3U);
  CHECK(segment.sourceOffset == 2U);
  CHECK(!table.next(cursor, segment));

  CHECK(table.beginSpan(RegisterTable::HoldingRegisters, 102U, 6U, cursor) ==
        RoutedSpanStatus::Gap);
  CHECK(!table.next(cursor, segment));

  EndpointRoute mismatch = makeHoldingRoute(1U, 0U, 0U, 2U);
  mismatch.downstream[static_cast<uint8_t>(RegisterTable::HoldingRegisters)].count = 1U;
  CHECK(RouteTableView(&mismatch, 1U).validate() ==
        RouteTableStatus::CountMismatch);

  EndpointRoute overflow = makeHoldingRoute(1U, 65535U, 0U, 2U);
  CHECK(RouteTableView(&overflow, 1U).validate() ==
        RouteTableStatus::RangeOverflow);

  EndpointRoute overlap[2];
  overlap[0] = makeHoldingRoute(1U, 10U, 0U, 4U);
  overlap[1] = makeHoldingRoute(2U, 13U, 0U, 2U);
  CHECK(RouteTableView(overlap, 2U).validate() == RouteTableStatus::Overlap);

  EndpointRoute withEmptyMiddle[3];
  withEmptyMiddle[0] = makeHoldingRoute(1U, 0U, 0U, 4U);
  // Empty routes still retain their sorted insertion point. This lets sparse
  // tables use the exact same binary-search path as fully populated tables.
  withEmptyMiddle[1].upstream[
      static_cast<uint8_t>(RegisterTable::HoldingRegisters)] =
      AddressRange(4U, 0U);
  withEmptyMiddle[2] = makeHoldingRoute(3U, 4U, 0U, 4U);
  RouteTableView zeroSafe(withEmptyMiddle, 3U);
  CHECK(zeroSafe.validate() == RouteTableStatus::Valid);
  CHECK(zeroSafe.locate(
      RegisterTable::HoldingRegisters, 1U, routeIndex));
  CHECK(routeIndex == 0U);
  CHECK(zeroSafe.locate(
      RegisterTable::HoldingRegisters, 6U, routeIndex));
  CHECK(routeIndex == 2U);

  EndpointRoute misplacedEmpty[3];
  misplacedEmpty[0] = makeHoldingRoute(1U, 0U, 0U, 4U);
  // The default start of zero is behind the first range's end, so accepting
  // this as a midpoint would make binary lookup incorrectly skip route zero.
  misplacedEmpty[2] = makeHoldingRoute(3U, 4U, 0U, 4U);
  CHECK(RouteTableView(misplacedEmpty, 3U).validate() ==
        RouteTableStatus::Unsorted);
}

void testCacheImage() {
  bool visible[8] = {false, false, false, false, false, false, false, false};
  bool applied[8] = {false, false, false, false, false, false, false, false};
  DesiredAppliedCache<bool> cache(
      MutableImageView<bool>(visible, 8U),
      MutableImageView<bool>(applied, 8U));
  CHECK(cache.validShape());

  bool snapshot[3] = {true, false, true};
  CHECK(cache.captureDesired(2U, snapshot, 3U));
  snapshot[0] = false;
  snapshot[2] = false;
  CHECK(visible[2]);
  CHECK(!visible[3]);
  CHECK(visible[4]);

  const bool firstApplied[3] = {true, true, false};
  CHECK(cache.markApplied(2U, firstApplied, 3U));
  CHECK(cache.restore(2U, 3U));
  CHECK(visible[2]);
  CHECK(visible[3]);
  CHECK(!visible[4]);

  CHECK(!cache.captureDesired(7U, firstApplied, 2U));
  CHECK(!cache.restore(8U, 1U));
  CHECK(!cache.restore(0U, 0U));

  uint16_t visibleRegisters[4] = {1U, 2U, 3U, 4U};
  uint16_t appliedRegisters[4] = {10U, 20U, 30U, 40U};
  DesiredAppliedCache<uint16_t> registers(
      MutableImageView<uint16_t>(visibleRegisters, 4U),
      MutableImageView<uint16_t>(appliedRegisters, 4U));
  CHECK(registers.restore(1U, 2U));
  CHECK(visibleRegisters[0] == 1U);
  CHECK(visibleRegisters[1] == 20U);
  CHECK(visibleRegisters[2] == 30U);
  CHECK(visibleRegisters[3] == 4U);
}

struct FakeBackend {
  typedef int Result;

  enum Call : uint8_t {
    None = 0U,
    ReadCoils,
    ReadDiscreteInputs,
    ReadHoldingRegisters,
    ReadInputRegisters,
    WriteSingleCoil,
    WriteSingleHoldingRegister,
    WriteMultipleCoils,
    WriteMultipleHoldingRegisters,
  };

  Call call;
  unsigned callCount;
  uint8_t exception;
  const DownstreamRequest* requestSeen;

  FakeBackend()
      : call(None), callCount(0U), exception(0x2AU), requestSeen(nullptr) {}

  Result invalidQuantityResult() const { return -2; }
  Result invalidBufferResult() const { return -3; }
  Result invalidOperationResult() const { return -4; }
  uint8_t exceptionCode() const { return exception; }

  Result note(Call next, const DownstreamRequest& request) {
    call = next;
    ++callCount;
    requestSeen = &request;
    return static_cast<Result>(100 + next);
  }

  Result readCoils(const DownstreamRequest& r) { return note(ReadCoils, r); }
  Result readDiscreteInputs(const DownstreamRequest& r) {
    return note(ReadDiscreteInputs, r);
  }
  Result readHoldingRegisters(const DownstreamRequest& r) {
    return note(ReadHoldingRegisters, r);
  }
  Result readInputRegisters(const DownstreamRequest& r) {
    return note(ReadInputRegisters, r);
  }
  Result writeSingleCoil(const DownstreamRequest& r) {
    return note(WriteSingleCoil, r);
  }
  Result writeSingleHoldingRegister(const DownstreamRequest& r) {
    return note(WriteSingleHoldingRegister, r);
  }
  Result writeMultipleCoils(const DownstreamRequest& r) {
    return note(WriteMultipleCoils, r);
  }
  Result writeMultipleHoldingRegisters(const DownstreamRequest& r) {
    return note(WriteMultipleHoldingRegisters, r);
  }
};

void testDownstreamExecutor() {
  bool coils[4] = {};
  uint16_t registers[4] = {};
  int context = 0;
  FakeBackend backend;
  DownstreamRequest request;
  request.sequence = 73U;
  request.endpointId = 9U;
  request.startAddress = 41U;
  request.quantity = 4U;
  request.coilBuffer = coils;
  request.registerBuffer = registers;
  request.coilValue = true;
  request.registerValue = 0x1234U;
  request.consistencyContext = &context;

  const DownstreamOperation operations[] = {
      DownstreamOperation::ReadCoils,
      DownstreamOperation::ReadDiscreteInputs,
      DownstreamOperation::ReadHoldingRegisters,
      DownstreamOperation::ReadInputRegisters,
      DownstreamOperation::WriteSingleCoil,
      DownstreamOperation::WriteSingleHoldingRegister,
      DownstreamOperation::WriteMultipleCoils,
      DownstreamOperation::WriteMultipleHoldingRegisters,
  };
  for(uint8_t index = 0U; index < 8U; ++index){
    request.operation = operations[index];
    const DownstreamCompletion<int> completion =
        executeDownstreamRequest(backend, request);
    CHECK(backend.callCount == static_cast<unsigned>(index + 1U));
    CHECK(backend.call == static_cast<FakeBackend::Call>(index + 1U));
    CHECK(backend.requestSeen == &request);
    CHECK(completion.sequence == 73U);
    CHECK(completion.result == static_cast<int>(101 + index));
    CHECK(completion.exceptionCode == 0x2AU);
  }

  const unsigned beforeInvalid = backend.callCount;
  request.operation = DownstreamOperation::ReadCoils;
  request.quantity = 0U;
  CHECK(executeDownstreamRequest(backend, request).result == -2);
  CHECK(backend.callCount == beforeInvalid);

  request.quantity = 1U;
  request.coilBuffer = nullptr;
  CHECK(executeDownstreamRequest(backend, request).result == -3);
  CHECK(backend.callCount == beforeInvalid);

  request.operation = static_cast<DownstreamOperation>(0xFFU);
  CHECK(executeDownstreamRequest(backend, request).result == -4);
  CHECK(backend.callCount == beforeInvalid);

  request.operation = DownstreamOperation::WriteSingleCoil;
  request.quantity = 0U;
  const DownstreamCompletion<int> prevalidated =
      executeValidatedDownstreamRequest(backend, request);
  CHECK(prevalidated.result == 105);
  CHECK(backend.callCount == beforeInvalid + 1U);
}

}  // namespace

int main() {
  testRouteValidationAndTranslation();
  testCacheImage();
  testDownstreamExecutor();
  printf("ModbusRTUStoreForwardBridge native checks: %u\n", g_checks);
  return 0;
}
