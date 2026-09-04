#include <modbus_rtu_bridge/StoreForwardBridge.h>

#include <chrono>
#include <stdint.h>
#include <stdio.h>

namespace {

using namespace ModbusRTUBridge;

static const uint32_t kIterations = 50000UL;
static const uint8_t kSamples = 21U;
static const uint16_t kLedgerCapacity = 16U;
volatile uint32_t g_sink = 0UL;

#if defined(__GNUC__)
#define MBUS_BRIDGE_FACADE_PERF __attribute__((noinline, aligned(64)))
#else
#define MBUS_BRIDGE_FACADE_PERF
#endif

inline uint32_t fold(uint32_t digest,
                     const PlannedWriteRequest& request,
                     CompletionNotice notice) {
  digest ^= request.identity.requestSequence;
  digest *= 16777619UL;
  digest ^= static_cast<uint32_t>(request.endpointId) +
            static_cast<uint32_t>(request.downstreamStart) +
            static_cast<uint32_t>(request.quantity) +
            static_cast<uint32_t>(request.holdingValue) +
            static_cast<uint32_t>(notice);
  return digest * 16777619UL;
}

MBUS_BRIDGE_FACADE_PERF double runDirect(
    const ForwardPlanner& planner,
    const uint16_t* value,
    uint32_t& digestOut) {
  uint32_t digest = 2166136261UL;
  uint32_t sequence = 0UL;
  const SessionStateView session(true, 1U, 1U, SessionPhase::Ready);
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for(uint32_t iteration = 0UL; iteration < kIterations; ++iteration){
    const uint16_t token = static_cast<uint16_t>(
        (iteration % 65535UL) + 1UL);
    const HoldingIngressWorkView work = makeHoldingIngressWork(
        WorkIdentity(token, 1U), 0U, 1U,
        IngressDelivery::Tracked, value);
    HoldingForwardCursor cursor;
    if(planner.begin(work, session, cursor) != ForwardPlanStatus::Ready){
      return 0.0;
    }
    sequence = nextNonZeroSequence32(sequence);
    PlannedWriteRequest request;
    if(planner.next(cursor, sequence, request) !=
       ForwardNextStatus::Planned){
      return 0.0;
    }
    CompletionAggregate aggregate(work.identity, work.delivery);
    const CompletionDecision completion = resolveCompletion(
        request,
        CompletionRecord(request.identity, DownstreamOutcome::Applied),
        session,
        aggregate);
    if(!completion.current() || !aggregate.complete){
      return 0.0;
    }
    digest = fold(digest, request, completion.notice);
  }
  const std::chrono::steady_clock::time_point end =
      std::chrono::steady_clock::now();
  digestOut = digest;
  g_sink += digest;
  const std::chrono::duration<double, std::nano> elapsed = end - start;
  return elapsed.count() / static_cast<double>(kIterations);
}

MBUS_BRIDGE_FACADE_PERF double runFacade(
    const ForwardPlanner& planner,
    const uint16_t* value,
    uint32_t& digestOut) {
  StoreForwardWorkSlot workStorage[1];
  CompletionLedgerSlot ledgerStorage[1];
  CompletionLedger ledger(ledgerStorage, 1U);
  StoreForwardBridge bridge(
      planner, workStorage, 1U, ledger,
      PollPlanner(PollPlannerOptions(0xFFFFU)));
  uint32_t digest = 2166136261UL;
  const SessionStateView session(true, 1U, 1U, SessionPhase::Ready);
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for(uint32_t iteration = 0UL; iteration < kIterations; ++iteration){
    const uint16_t token = static_cast<uint16_t>(
        (iteration % 65535UL) + 1UL);
    const HoldingIngressWorkView work = makeHoldingIngressWork(
        WorkIdentity(token, 1U), 0U, 1U,
        IngressDelivery::Tracked, value);
    if(bridge.admitWork(work, session).status !=
       StoreForwardAdmitStatus::Admitted){
      return 0.0;
    }
    StoreForwardAction action;
    if(bridge.nextAction(session, 0UL, nullptr, 0U, action) !=
           StoreForwardNextStatus::Ready ||
       bridge.admitAction(action, session, nullptr, 0U) !=
           StoreForwardActionAdmitStatus::Admitted){
      return 0.0;
    }
    const StoreForwardCompletion completion = bridge.complete(
        action, DownstreamOutcome::Applied, session);
    if(completion.status != StoreForwardCompleteStatus::Completed ||
       !completion.workRetired){
      return 0.0;
    }
    digest = fold(digest, action.write, completion.decision.notice);
  }
  const std::chrono::steady_clock::time_point end =
      std::chrono::steady_clock::now();
  digestOut = digest;
  g_sink += digest;
  const std::chrono::duration<double, std::nano> elapsed = end - start;
  return elapsed.count() / static_cast<double>(kIterations);
}

// Measure the public ledger's bounded linear lookup at both ends of the same
// non-trivial, completely occupied slot array. Setup is outside the timed
// region; each iteration records and resolves the same amount of work.
MBUS_BRIDGE_FACADE_PERF double runLedgerSlot(
    uint16_t targetSlot,
    uint32_t& digestOut) {
  if(targetSlot >= kLedgerCapacity){
    return 0.0;
  }
  CompletionLedgerSlot storage[kLedgerCapacity];
  CompletionLedger ledger(storage, kLedgerCapacity);
  const WorkIdentity target(1U, 1U);
  for(uint16_t index = 0U; index < kLedgerCapacity; ++index){
    const WorkIdentity work = index == targetSlot
        ? target
        : WorkIdentity(static_cast<uint16_t>(index + 2U), 1U);
    if(ledger.reserve(work, IngressDelivery::Tracked) !=
       LedgerReserveStatus::Reserved){
      return 0.0;
    }
  }

  uint16_t value = 0x4321U;
  PlannedWriteRequest request;
  request.delivery = IngressDelivery::Tracked;
  request.table = RegisterTable::HoldingRegisters;
  request.operation = DownstreamOperation::WriteSingleHoldingRegister;
  request.endpointId = 3U;
  request.quantity = 1U;
  request.holdingValues = &value;
  request.holdingValue = value;
  request.finalFragment = false;
  const SessionStateView session(true, 1U, 1U, SessionPhase::Ready);
  uint32_t digest = 2166136261UL;

  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for(uint32_t iteration = 0UL; iteration < kIterations; ++iteration){
    request.identity = RequestIdentity(
        target,
        iteration + 1UL,
        static_cast<uint16_t>(iteration));
    if(ledger.recordIssued(request) != LedgerIssueStatus::Recorded){
      return 0.0;
    }
    const LedgerResolution resolution = ledger.resolve(
        request,
        CompletionRecord(request.identity, DownstreamOutcome::Applied),
        session);
    if(resolution.status != LedgerResolveStatus::Resolved ||
       resolution.summary.outstandingRequests != 0U){
      return 0.0;
    }
    digest ^= resolution.decision.identity.requestSequence;
    digest *= 16777619UL;
  }
  const std::chrono::steady_clock::time_point end =
      std::chrono::steady_clock::now();
  digestOut = digest;
  g_sink += digest;
  const std::chrono::duration<double, std::nano> elapsed = end - start;
  return elapsed.count() / static_cast<double>(kIterations);
}

// Compare the convenience scan with the exact lower-level planner sequence it
// wraps. Both lanes construct, advance twice and commit one poll per iteration;
// the digest also proves identical cursor/timestamp publication.
MBUS_BRIDGE_FACADE_PERF double runRawPollIndexScan(uint32_t& digestOut) {
  uint32_t digest = 2166136261UL;
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for(uint32_t iteration = 0UL; iteration < kIterations; ++iteration){
    const uint16_t initial = static_cast<uint16_t>(iteration & 3UL);
    PollPlanner planner;
    PollPlannerState state;
    state.nextPollIndex = initial;
    state.lastPollAt = iteration;
    PollScanCursor cursor;
    planner.beginPollScan(4U, state, cursor);
    uint16_t index = 0U;
    if(planner.nextIndex(cursor, index) != PollScanIndexStatus::Candidate ||
       planner.nextIndex(cursor, index) != PollScanIndexStatus::Candidate ||
       !planner.commit(
           PollSelection::fromCandidateSet(
               ScheduledActionKind::Poll, index, 4U),
           4U, iteration + 1UL, state)){
      return 0.0;
    }
    digest ^= static_cast<uint32_t>(state.nextPollIndex);
    digest *= 16777619UL;
    digest ^= state.lastPollAt;
    digest *= 16777619UL;
  }
  const std::chrono::steady_clock::time_point end =
      std::chrono::steady_clock::now();
  digestOut = digest;
  g_sink += digest;
  const std::chrono::duration<double, std::nano> elapsed = end - start;
  return elapsed.count() / static_cast<double>(kIterations);
}

MBUS_BRIDGE_FACADE_PERF double runConveniencePollIndexScan(
    uint32_t& digestOut) {
  uint32_t digest = 2166136261UL;
  const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  for(uint32_t iteration = 0UL; iteration < kIterations; ++iteration){
    PollIndexScan scan(
        4U, static_cast<uint16_t>(iteration & 3UL), iteration);
    uint16_t index = 0U;
    if(!scan.next(index) || !scan.next(index) ||
       !scan.commitConsumed(index, 4U, iteration + 1UL)){
      return 0.0;
    }
    digest ^= static_cast<uint32_t>(scan.nextPollIndex());
    digest *= 16777619UL;
    digest ^= scan.lastPollAt();
    digest *= 16777619UL;
  }
  const std::chrono::steady_clock::time_point end =
      std::chrono::steady_clock::now();
  digestOut = digest;
  g_sink += digest;
  const std::chrono::duration<double, std::nano> elapsed = end - start;
  return elapsed.count() / static_cast<double>(kIterations);
}

void sort(double* values, uint8_t count) {
  for(uint8_t index = 1U; index < count; ++index){
    const double value = values[index];
    uint8_t insertion = index;
    while(insertion != 0U && values[insertion - 1U] > value){
      values[insertion] = values[insertion - 1U];
      --insertion;
    }
    values[insertion] = value;
  }
}

}  // namespace

int main() {
  EndpointRoute route;
  route.endpointId = 3U;
  const uint8_t holding =
      static_cast<uint8_t>(RegisterTable::HoldingRegisters);
  route.upstream[holding] = AddressRange(0U, 1U);
  route.downstream[holding] = AddressRange(20U, 1U);
  const RouteTableView routes(&route, 1U);
  if(routes.validate() != RouteTableStatus::Valid){
    return 2;
  }
  const ForwardPlanner planner(routes, ForwardPlanOptions(1U, 1U));
  const uint16_t value = 0x1234U;

  uint32_t directDigest = 0UL;
  uint32_t facadeDigest = 0UL;
  uint32_t firstLedgerDigest = 0UL;
  uint32_t worstLedgerDigest = 0UL;
  uint32_t rawPollDigest = 0UL;
  uint32_t conveniencePollDigest = 0UL;
  (void)runDirect(planner, &value, directDigest);
  (void)runFacade(planner, &value, facadeDigest);
  (void)runLedgerSlot(0U, firstLedgerDigest);
  (void)runLedgerSlot(
      static_cast<uint16_t>(kLedgerCapacity - 1U), worstLedgerDigest);
  (void)runRawPollIndexScan(rawPollDigest);
  (void)runConveniencePollIndexScan(conveniencePollDigest);
  if(directDigest != facadeDigest){
    return 3;
  }
  if(firstLedgerDigest != worstLedgerDigest){
    return 5;
  }
  if(rawPollDigest != conveniencePollDigest){
    return 6;
  }

  double ratios[kSamples];
  double ledgerRatios[kSamples];
  double pollScanRatios[kSamples];
  double directTotal = 0.0;
  double facadeTotal = 0.0;
  double firstLedgerTotal = 0.0;
  double worstLedgerTotal = 0.0;
  double rawPollTotal = 0.0;
  double conveniencePollTotal = 0.0;
  for(uint8_t sample = 0U; sample < kSamples; ++sample){
    double direct = 0.0;
    double facade = 0.0;
    double firstLedger = 0.0;
    double worstLedger = 0.0;
    double rawPoll = 0.0;
    double conveniencePoll = 0.0;
    if((sample & 1U) == 0U){
      direct = runDirect(planner, &value, directDigest);
      facade = runFacade(planner, &value, facadeDigest);
      firstLedger = runLedgerSlot(0U, firstLedgerDigest);
      worstLedger = runLedgerSlot(
          static_cast<uint16_t>(kLedgerCapacity - 1U), worstLedgerDigest);
      rawPoll = runRawPollIndexScan(rawPollDigest);
      conveniencePoll =
          runConveniencePollIndexScan(conveniencePollDigest);
    }else{
      conveniencePoll =
          runConveniencePollIndexScan(conveniencePollDigest);
      rawPoll = runRawPollIndexScan(rawPollDigest);
      worstLedger = runLedgerSlot(
          static_cast<uint16_t>(kLedgerCapacity - 1U), worstLedgerDigest);
      firstLedger = runLedgerSlot(0U, firstLedgerDigest);
      facade = runFacade(planner, &value, facadeDigest);
      direct = runDirect(planner, &value, directDigest);
    }
    if(direct == 0.0 || facade == 0.0 || firstLedger == 0.0 ||
       worstLedger == 0.0 || rawPoll == 0.0 ||
       conveniencePoll == 0.0 || directDigest != facadeDigest ||
       firstLedgerDigest != worstLedgerDigest ||
       rawPollDigest != conveniencePollDigest){
      return 4;
    }
    ratios[sample] = facade / direct;
    ledgerRatios[sample] = worstLedger / firstLedger;
    pollScanRatios[sample] = conveniencePoll / rawPoll;
    directTotal += direct;
    facadeTotal += facade;
    firstLedgerTotal += firstLedger;
    worstLedgerTotal += worstLedger;
    rawPollTotal += rawPoll;
    conveniencePollTotal += conveniencePoll;
  }
  sort(ratios, kSamples);
  sort(ledgerRatios, kSamples);
  sort(pollScanRatios, kSamples);
  const double medianRatio = ratios[kSamples / 2U];
  const double medianLedgerRatio = ledgerRatios[kSamples / 2U];
  const double medianPollScanRatio = pollScanRatios[kSamples / 2U];
  printf("cooperative facade: direct=%.2f ns facade=%.2f ns "
         "paired_median=%.4f work_slot=%luB ledger_slot=%luB action=%luB "
         "sink=%lu\n",
         directTotal / kSamples,
         facadeTotal / kSamples,
         medianRatio,
         static_cast<unsigned long>(sizeof(StoreForwardWorkSlot)),
         static_cast<unsigned long>(sizeof(CompletionLedgerSlot)),
         static_cast<unsigned long>(sizeof(StoreForwardAction)),
         static_cast<unsigned long>(g_sink));
  printf("completion ledger: first=%.2f ns last_of_%u=%.2f ns "
         "paired_median=%.4f\n",
         firstLedgerTotal / kSamples,
         static_cast<unsigned>(kLedgerCapacity),
         worstLedgerTotal / kSamples,
         medianLedgerRatio);
  printf("poll index scan: raw=%.2f ns convenience=%.2f ns "
         "paired_median=%.4f scan=%luB\n",
         rawPollTotal / kSamples,
         conveniencePollTotal / kSamples,
         medianPollScanRatio,
         static_cast<unsigned long>(sizeof(PollIndexScan)));

  // One facade cycle intentionally adds an admission re-plan, bounded ledger
  // scan, exact action validation, and queue/fairness state transitions. This
  // broad ceiling catches gross extra scans/copies while keeping nanosecond
  // host noise from becoming flaky. It is a regression guard, not a prediction
  // of serial-bus timing or a small MCU's instruction costs.
  const bool boundedStorage = sizeof(StoreForwardWorkSlot) <= 64U &&
                              sizeof(CompletionLedgerSlot) <= 96U &&
                              sizeof(StoreForwardAction) <= 160U &&
                              sizeof(PollIndexScan) <= 16U;
  // A sixteen-slot last-entry lookup is expected to cost more than the first
  // entry because the public ledger is intentionally bounded-linear. The
  // ceiling is broad enough for host noise but fails accidental unbounded or
  // quadratic work in the record/resolve hot path.
  const bool boundedLedgerLookup = medianLedgerRatio <= 6.0;
  const bool boundedPollScan = medianPollScanRatio <= 1.5;
  return boundedStorage && medianRatio <= 9.0 && boundedLedgerLookup &&
         boundedPollScan ? 0 : 1;
}
