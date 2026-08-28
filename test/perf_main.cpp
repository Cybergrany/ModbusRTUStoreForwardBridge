#include <modbus_rtu_bridge/ForwardPlan.h>
#include <modbus_rtu_bridge/RouteTable.h>

#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

using namespace ModbusRTUBridge;

static const uint16_t kRouteCount = 9U;
static const uint32_t kIterations = 2000000UL;
static const uint32_t kPlanIterations = 200000UL;
static const uint8_t kSampleCount = 31U;
volatile uint32_t g_sink = 0U;

#if defined(__GNUC__)
#define MBUS_RTU_BRIDGE_PERF_LOOKUP __attribute__((noinline, aligned(64)))
#else
#define MBUS_RTU_BRIDGE_PERF_LOOKUP
#endif

// Small standalone implementation of the documented lookup contract. It is a
// stable performance baseline, not a second production implementation.
MBUS_RTU_BRIDGE_PERF_LOOKUP bool baselineLocate(const EndpointRoute* routes,
                                  uint16_t routeCount,
                                  uint16_t address,
                                  uint16_t& indexOut) {
  int16_t low = 0;
  int16_t high = static_cast<int16_t>(routeCount) - 1;
  const uint8_t table = static_cast<uint8_t>(RegisterTable::HoldingRegisters);
  while(low <= high){
    const int16_t middle =
        static_cast<int16_t>(low + ((high - low) / 2));
    const AddressRange& range = routes[static_cast<uint16_t>(middle)].upstream[table];
    if(static_cast<uint32_t>(address) < static_cast<uint32_t>(range.start)){
      high = static_cast<int16_t>(middle - 1);
      continue;
    }
    if(static_cast<uint32_t>(address) >= range.end()){
      low = static_cast<int16_t>(middle + 1);
      continue;
    }
    if(range.count != 0U){
      indexOut = static_cast<uint16_t>(middle);
      return true;
    }
    low = static_cast<int16_t>(middle + 1);
  }
  return false;
}

// Keep both lookup bodies out of the timing-lane template and give them the
// same code alignment. Otherwise unrelated compiler layout can make one
// inlined loop straddle a cache/decoder boundary and intermittently look more
// than five percent slower even when the lookup bodies are equivalent.
MBUS_RTU_BRIDGE_PERF_LOOKUP bool candidateLocate(const RouteTableView& table,
                                     uint16_t address,
                                     uint16_t& indexOut) {
  return table.locate(
      RegisterTable::HoldingRegisters, address, indexOut);
}

inline uint32_t foldPlannedRequest(uint32_t digest,
                                   uint16_t routeIndex,
                                   uint8_t endpointId,
                                   uint16_t downstreamStart,
                                   uint16_t quantity,
                                   uint16_t sourceOffset,
                                   uint16_t firstValue,
                                   uint32_t sequence,
                                   uint16_t fragmentIndex) {
  digest ^= sequence + static_cast<uint32_t>(fragmentIndex);
  digest *= 16777619UL;
  digest ^= static_cast<uint32_t>(routeIndex) +
            static_cast<uint32_t>(endpointId) +
            static_cast<uint32_t>(downstreamStart) +
            static_cast<uint32_t>(quantity) +
            static_cast<uint32_t>(sourceOffset) +
            static_cast<uint32_t>(firstValue);
  return digest * 16777619UL;
}

// Bounded reference for one fully preflighted latest-state plan. It performs
// the same route lookups, endpoint/quantity splits, and digest work as the
// public planner without constructing its request/cursor types.
MBUS_RTU_BRIDGE_PERF_LOOKUP uint32_t baselinePlan(
    const RouteTableView& table,
    const HoldingIngressWorkView& work,
    uint16_t maxQuantity,
    uint32_t sequence) {
  if(table.inspectSpan(RegisterTable::HoldingRegisters,
                       work.start,
                       work.count) != RoutedSpanStatus::Complete){
    return 0U;
  }

  uint16_t nextAddress = work.start;
  uint16_t remaining = work.count;
  uint16_t sourceOffset = 0U;
  uint16_t fragmentIndex = 0U;
  uint32_t digest = 2166136261UL;
  while(remaining != 0U){
    uint16_t routeIndex = 0U;
    if(!table.locate(RegisterTable::HoldingRegisters,
                     nextAddress,
                     routeIndex)){
      return 0U;
    }
    const EndpointRoute& route = table.data()[routeIndex];
    const AddressRange& upstream = route.upstream[
        static_cast<uint8_t>(RegisterTable::HoldingRegisters)];
    const AddressRange& downstream = route.downstream[
        static_cast<uint8_t>(RegisterTable::HoldingRegisters)];
    const uint16_t routeOffset =
        static_cast<uint16_t>(nextAddress - upstream.start);
    const uint16_t available =
        static_cast<uint16_t>(upstream.count - routeOffset);
    uint16_t quantity = remaining < available ? remaining : available;
    if(quantity > maxQuantity){
      quantity = maxQuantity;
    }
    sequence = nextNonZeroSequence32(sequence);
    digest = foldPlannedRequest(
        digest,
        routeIndex,
        route.endpointId,
        static_cast<uint16_t>(downstream.start + routeOffset),
        quantity,
        sourceOffset,
        work.snapshot.data()[sourceOffset],
        sequence,
        fragmentIndex);
    nextAddress = static_cast<uint16_t>(nextAddress + quantity);
    remaining = static_cast<uint16_t>(remaining - quantity);
    sourceOffset = static_cast<uint16_t>(sourceOffset + quantity);
    ++fragmentIndex;
  }
  return digest;
}

MBUS_RTU_BRIDGE_PERF_LOOKUP uint32_t candidatePlan(
    const ForwardPlanner& planner,
    const HoldingIngressWorkView& work,
    uint32_t sequence) {
  HoldingForwardCursor cursor;
  if(planner.begin(work, cursor) != ForwardPlanStatus::Ready){
    return 0U;
  }

  uint32_t digest = 2166136261UL;
  while(cursor.active){
    sequence = nextNonZeroSequence32(sequence);
    PlannedWriteRequest request;
    if(planner.next(cursor, sequence, request) !=
       ForwardNextStatus::Planned){
      return 0U;
    }
    digest = foldPlannedRequest(
        digest,
        request.routeIndex,
        request.endpointId,
        request.downstreamStart,
        request.quantity,
        request.sourceOffset,
        request.holdingValue,
        request.identity.requestSequence,
        request.identity.fragmentIndex);
  }
  return digest;
}

template<typename Locate>
double runLane(Locate locate, const uint16_t* addresses, uint16_t addressCount) {
  uint32_t local = 0U;
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for(uint32_t iteration = 0U; iteration < kIterations; ++iteration){
    uint16_t index = 0U;
    const uint16_t address = addresses[iteration % addressCount];
    const bool found = locate(address, index);
    local += found ? static_cast<uint32_t>(index + 1U) : 17U;
  }
  const std::chrono::steady_clock::time_point end =
      std::chrono::steady_clock::now();
  g_sink += local;
  const std::chrono::duration<double, std::nano> elapsed = end - start;
  return elapsed.count() / static_cast<double>(kIterations);
}

template<typename Plan>
double runPlanLane(Plan plan) {
  uint32_t local = 0U;
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for(uint32_t iteration = 0U; iteration < kPlanIterations; ++iteration){
    local += plan(iteration);
  }
  const std::chrono::steady_clock::time_point end =
      std::chrono::steady_clock::now();
  g_sink += local;
  const std::chrono::duration<double, std::nano> elapsed = end - start;
  return elapsed.count() / static_cast<double>(kPlanIterations);
}

void sort(double* values, uint8_t count) {
  for(uint8_t i = 1U; i < count; ++i){
    const double value = values[i];
    uint8_t j = i;
    while(j != 0U && values[j - 1U] > value){
      values[j] = values[j - 1U];
      --j;
    }
    values[j] = value;
  }
}

}  // namespace

int main() {
  EndpointRoute routes[kRouteCount];
  for(uint16_t index = 0U; index < kRouteCount; ++index){
    routes[index].endpointId = static_cast<uint8_t>(index + 1U);
    const uint16_t start = static_cast<uint16_t>(index * 32U);
    routes[index].upstream[
        static_cast<uint8_t>(RegisterTable::HoldingRegisters)] =
        AddressRange(start, 24U);
    routes[index].downstream[
        static_cast<uint8_t>(RegisterTable::HoldingRegisters)] =
        AddressRange(0U, 24U);
  }
  const RouteTableView table(routes, kRouteCount);
  if(table.validate() != RouteTableStatus::Valid){
    return 2;
  }

  uint16_t addresses[64];
  for(uint16_t index = 0U; index < 64U; ++index){
    addresses[index] = static_cast<uint16_t>((index * 37U) % 288U);
  }

  // Warm both lanes before sampling. This avoids treating host CPU frequency
  // ramp-up as a library regression while the paired, alternating samples
  // below still expose a sustained lookup cost.
  (void)runLane(
      [&](uint16_t address, uint16_t& index) {
        return baselineLocate(routes, kRouteCount, address, index);
      },
      addresses,
      64U);
  (void)runLane(
      [&](uint16_t address, uint16_t& index) {
        return candidateLocate(table, address, index);
      },
      addresses,
      64U);

  double ratios[kSampleCount];
  double baselineTotal = 0.0;
  double candidateTotal = 0.0;
  for(uint8_t sample = 0U; sample < kSampleCount; ++sample){
    double baseline = 0.0;
    double candidate = 0.0;
    if((sample & 1U) == 0U){
      baseline = runLane(
          [&](uint16_t address, uint16_t& index) {
            return baselineLocate(routes, kRouteCount, address, index);
          },
          addresses,
          64U);
      candidate = runLane(
          [&](uint16_t address, uint16_t& index) {
            return candidateLocate(table, address, index);
          },
          addresses,
          64U);
    }else{
      candidate = runLane(
          [&](uint16_t address, uint16_t& index) {
            return candidateLocate(table, address, index);
          },
          addresses,
          64U);
      baseline = runLane(
          [&](uint16_t address, uint16_t& index) {
            return baselineLocate(routes, kRouteCount, address, index);
          },
          addresses,
          64U);
    }
    ratios[sample] = candidate / baseline;
    baselineTotal += baseline;
    candidateTotal += candidate;
  }
  sort(ratios, kSampleCount);
  const double medianRatio = ratios[kSampleCount / 2U];
  printf("route lookup: baseline=%.2f ns candidate=%.2f ns paired_median=%.4f sink=%lu\n",
         baselineTotal / kSampleCount,
         candidateTotal / kSampleCount,
         medianRatio,
         static_cast<unsigned long>(g_sink));

  EndpointRoute planRoutes[4];
  for(uint16_t index = 0U; index < 4U; ++index){
    planRoutes[index].endpointId = static_cast<uint8_t>(index + 20U);
    planRoutes[index].upstream[
        static_cast<uint8_t>(RegisterTable::HoldingRegisters)] =
        AddressRange(static_cast<uint16_t>(index * 16U), 16U);
    planRoutes[index].downstream[
        static_cast<uint8_t>(RegisterTable::HoldingRegisters)] =
        AddressRange(static_cast<uint16_t>(index * 3U), 16U);
  }
  const RouteTableView planTable(planRoutes, 4U);
  if(planTable.validate() != RouteTableStatus::Valid){
    return 2;
  }
  uint16_t planValues[48];
  for(uint16_t index = 0U; index < 48U; ++index){
    planValues[index] = static_cast<uint16_t>(index * 17U + 3U);
  }
  const HoldingIngressWorkView planWork = makeHoldingIngressWork(
      WorkIdentity(), 8U, 48U, IngressDelivery::LatestState, planValues);
  const ForwardPlanner planner(
      planTable, ForwardPlanOptions(7U, 7U));
  if(baselinePlan(planTable, planWork, 7U, 1U) !=
     candidatePlan(planner, planWork, 1U)){
    return 3;
  }

  (void)runPlanLane([&](uint32_t sequence) {
    return baselinePlan(planTable, planWork, 7U, sequence);
  });
  (void)runPlanLane([&](uint32_t sequence) {
    return candidatePlan(planner, planWork, sequence);
  });

  double planRatios[kSampleCount];
  double planBaselineTotal = 0.0;
  double planCandidateTotal = 0.0;
  for(uint8_t sample = 0U; sample < kSampleCount; ++sample){
    double baseline = 0.0;
    double candidate = 0.0;
    if((sample & 1U) == 0U){
      baseline = runPlanLane([&](uint32_t sequence) {
        return baselinePlan(planTable, planWork, 7U, sequence);
      });
      candidate = runPlanLane([&](uint32_t sequence) {
        return candidatePlan(planner, planWork, sequence);
      });
    }else{
      candidate = runPlanLane([&](uint32_t sequence) {
        return candidatePlan(planner, planWork, sequence);
      });
      baseline = runPlanLane([&](uint32_t sequence) {
        return baselinePlan(planTable, planWork, 7U, sequence);
      });
    }
    planRatios[sample] = candidate / baseline;
    planBaselineTotal += baseline;
    planCandidateTotal += candidate;
  }
  sort(planRatios, kSampleCount);
  const double planMedianRatio = planRatios[kSampleCount / 2U];
  printf("forward planner: baseline=%.2f ns candidate=%.2f ns paired_median=%.4f sink=%lu\n",
         planBaselineTotal / kSampleCount,
         planCandidateTotal / kSampleCount,
         planMedianRatio,
         static_cast<unsigned long>(g_sink));

  // The planner constructs rich request identities and typed payload views on
  // top of the bounded reference. This generous relative limit catches an
  // accidental extra scan/copy without turning host nanosecond noise into a
  // flaky gate.
  return medianRatio <= 1.05 && planMedianRatio <= 1.75 ? 0 : 1;
}
