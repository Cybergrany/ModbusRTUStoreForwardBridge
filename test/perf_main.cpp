#include <modbus_rtu_bridge/RouteTable.h>

#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace {

using namespace ModbusRTUBridge;

static const uint16_t kRouteCount = 9U;
static const uint32_t kIterations = 2000000UL;
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
  return medianRatio <= 1.05 ? 0 : 1;
}
