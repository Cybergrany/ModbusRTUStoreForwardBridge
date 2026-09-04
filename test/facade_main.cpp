#include <modbus_rtu_bridge/CompletionLedger.h>
#include <modbus_rtu_bridge/FixedStorage.h>
#include <modbus_rtu_bridge/PollPlanner.h>
#include <modbus_rtu_bridge/StoreForwardBridge.h>
#include <modbus_rtu_bridge/adapters/ModbusRTUMasterBackend.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>

namespace {

using namespace ModbusRTUBridge;

static_assert(!std::is_copy_constructible<FixedRingQueue<uint16_t> >::value,
              "FixedRingQueue must not alias storage through copies");
static_assert(!std::is_copy_constructible<CompletionLedger>::value,
              "CompletionLedger must not alias storage through copies");
static_assert(!std::is_copy_constructible<StoreForwardBridge>::value,
              "StoreForwardBridge must not alias storage through copies");
static_assert(sizeof(PollIndexScan) <= 32U,
              "PollIndexScan must remain a small scalar-only hot-path view");

unsigned g_checks = 0U;
size_t g_allocations = 0U;

#define CHECK(condition)                                                     \
  do {                                                                       \
    ++g_checks;                                                              \
    if(!(condition)){                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n",                       \
              __FILE__, __LINE__, #condition);                              \
      exit(1);                                                               \
    }                                                                        \
  } while(false)

EndpointRoute makeHoldingRoute(uint8_t endpoint,
                               uint16_t upstream,
                               uint16_t downstream,
                               uint16_t count) {
  EndpointRoute route;
  route.endpointId = endpoint;
  const uint8_t table =
      static_cast<uint8_t>(RegisterTable::HoldingRegisters);
  route.upstream[table] = AddressRange(upstream, count);
  route.downstream[table] = AddressRange(downstream, count);
  return route;
}

DownstreamRequest makeHoldingPoll(uint8_t endpoint,
                                  uint16_t start,
                                  uint16_t* values,
                                  uint16_t count) {
  DownstreamRequest request;
  request.operation = DownstreamOperation::ReadHoldingRegisters;
  request.endpointId = endpoint;
  request.startAddress = start;
  request.quantity = count;
  request.registerBuffer = values;
  return request;
}

void testFixedStorageCapacityAndWrap() {
  FixedRingQueue<uint16_t> invalid(nullptr, 2U);
  CHECK(!invalid.validStorage());
  CHECK(!invalid.tryPush(1U));

  FixedRingQueue<uint16_t> disabled(nullptr, 0U);
  CHECK(disabled.validStorage());
  CHECK(disabled.empty());
  CHECK(disabled.full());
  CHECK(!disabled.tryPush(1U));

  uint16_t storage[3] = {};
  FixedRingQueue<uint16_t> queue(storage, 3U);
  CHECK(queue.validStorage());
  CHECK(queue.tryPush(10U));
  CHECK(queue.tryPush(20U));
  CHECK(queue.tryPush(30U));
  CHECK(queue.full());
  CHECK(!queue.tryPush(40U));
  CHECK(queue.front() != nullptr && *queue.front() == 10U);

  uint16_t removed = 0U;
  CHECK(queue.tryPop(removed));
  CHECK(removed == 10U);
  CHECK(queue.tryPush(40U));  // tail wraps to the first physical slot
  CHECK(queue.size() == 3U);
  CHECK(queue.at(0U) != nullptr && *queue.at(0U) == 20U);
  CHECK(queue.at(1U) != nullptr && *queue.at(1U) == 30U);
  CHECK(queue.at(2U) != nullptr && *queue.at(2U) == 40U);
  CHECK(queue.at(3U) == nullptr);
  CHECK(queue.tryPop(removed) && removed == 20U);
  CHECK(queue.tryPop(removed) && removed == 30U);
  CHECK(queue.tryPop(removed) && removed == 40U);
  CHECK(queue.empty());
  CHECK(queue.front() == nullptr);
  CHECK(!queue.popFront());

  CHECK(queue.tryPush(50U));
  queue.clear();
  CHECK(queue.empty());
  CHECK(queue.tryPush(60U));
  CHECK(queue.front() != nullptr && *queue.front() == 60U);

  // The same reusable queue stores complete proposed actions without owning
  // the snapshots/buffers referenced by those actions.
  StoreForwardAction actions[2];
  FixedRingQueue<StoreForwardAction> actionQueue(actions, 2U);
  StoreForwardAction action;
  action.kind = ScheduledActionKind::Poll;
  action.sequence = 7U;
  CHECK(actionQueue.tryPush(action));
  CHECK(actionQueue.front()->sequence == 7U);
}

void makeThreeFragmentPlan(ForwardPlanner& planner,
                           const HoldingIngressWorkView& work,
                           PlannedWriteRequest (&requests)[3]) {
  HoldingForwardCursor cursor;
  CHECK(planner.begin(work, cursor) == ForwardPlanStatus::Ready);
  CHECK(planner.next(cursor, 11U, requests[0]) == ForwardNextStatus::Planned);
  CHECK(planner.next(cursor, 12U, requests[1]) == ForwardNextStatus::Planned);
  CHECK(planner.next(cursor, 13U, requests[2]) == ForwardNextStatus::Planned);
  CHECK(requests[2].finalFragment);
}

void testCompletionLedgerCapacityOrderingAndOutcomes() {
  EndpointRoute route = makeHoldingRoute(4U, 100U, 0U, 5U);
  const RouteTableView routes(&route, 1U);
  CHECK(routes.validate() == RouteTableStatus::Valid);
  ForwardPlanner planner(routes, ForwardPlanOptions(8U, 2U));

  const uint16_t values1[] = {1U, 2U, 3U, 4U, 5U};
  const HoldingIngressWorkView work1 = makeHoldingIngressWork(
      WorkIdentity(1U, 7U), 100U, 5U,
      IngressDelivery::Tracked, values1);
  PlannedWriteRequest requests[3];
  makeThreeFragmentPlan(planner, work1, requests);

  CompletionLedgerSlot slots[2];
  CompletionLedger ledger(slots, 2U);
  CHECK(ledger.validStorage());
  CHECK(ledger.reserve(work1.identity, work1.delivery) ==
        LedgerReserveStatus::Reserved);
  CHECK(ledger.reserve(work1.identity, work1.delivery) ==
        LedgerReserveStatus::DuplicateWork);
  CHECK(ledger.reserve(WorkIdentity(), work1.delivery) ==
        LedgerReserveStatus::InvalidWork);

  const uint16_t values2[] = {9U};
  const HoldingIngressWorkView work2 = makeHoldingIngressWork(
      WorkIdentity(2U, 7U), 100U, 1U,
      IngressDelivery::SilentOrdered, values2);
  CHECK(ledger.reserve(work2.identity, work2.delivery) ==
        LedgerReserveStatus::Reserved);
  CHECK(ledger.full());
  CHECK(ledger.reserve(WorkIdentity(3U, 7U), IngressDelivery::Tracked) ==
        LedgerReserveStatus::Full);

  CHECK(ledger.recordIssued(requests[1]) == LedgerIssueStatus::OutOfOrder);
  CHECK(ledger.recordIssued(requests[0]) == LedgerIssueStatus::Recorded);
  CHECK(ledger.recordIssued(requests[1]) ==
        LedgerIssueStatus::OutstandingRequestExists);
  CHECK(ledger.release(work1.identity) == LedgerReleaseStatus::StillActive);

  WorkCompletionSummary summary;
  CHECK(ledger.summary(work1.identity, summary));
  CHECK(summary.outstandingRequests == 1U);
  CHECK(!summary.settled);
  CHECK(!summary.drained);
  CHECK(summary.outcome == WorkCompletionOutcome::Pending);

  const SessionStateView ready(true, 7U, 7U, SessionPhase::Ready);
  LedgerResolution resolved = ledger.resolve(
      requests[0],
      CompletionRecord(requests[0].identity,
                       DownstreamOutcome::DefinitelyNotSent),
      ready);
  CHECK(resolved.status == LedgerResolveStatus::Resolved);
  CHECK(resolved.decision.notice == CompletionNotice::WorkFailed);
  CHECK(resolved.summary.settled);
  CHECK(!resolved.summary.drained);
  CHECK(resolved.summary.outcome ==
        WorkCompletionOutcome::DefinitelyNotSent);
  CHECK(resolved.summary.outstandingRequests == 0U);

  CHECK(ledger.recordIssued(requests[1]) == LedgerIssueStatus::Recorded);
  resolved = ledger.resolve(
      requests[1],
      CompletionRecord(requests[1].identity,
                       DownstreamOutcome::SendUncertain),
      ready);
  CHECK(resolved.status == LedgerResolveStatus::Resolved);
  CHECK(resolved.decision.notice == CompletionNotice::None);
  CHECK(resolved.summary.outcome == WorkCompletionOutcome::UncertainSend);
  CHECK(!resolved.summary.drained);

  CHECK(ledger.recordIssued(requests[2]) == LedgerIssueStatus::Recorded);
  CHECK(ledger.recordIssued(requests[2]) == LedgerIssueStatus::PlanningClosed);
  resolved = ledger.resolve(
      requests[2],
      CompletionRecord(requests[2].identity,
                       DownstreamOutcome::FailedAfterSend),
      ready);
  CHECK(resolved.status == LedgerResolveStatus::Resolved);
  CHECK(resolved.summary.outcome == WorkCompletionOutcome::TerminalFailure);
  CHECK(resolved.summary.settled);
  CHECK(resolved.summary.drained);
  CHECK(resolved.summary.outstandingRequests == 0U);
  CHECK(ledger.release(work1.identity) == LedgerReleaseStatus::Released);

  HoldingForwardCursor secondCursor;
  CHECK(planner.begin(work2, secondCursor) == ForwardPlanStatus::Ready);
  PlannedWriteRequest secondRequest;
  CHECK(planner.next(secondCursor, 21U, secondRequest) ==
        ForwardNextStatus::Planned);
  CHECK(ledger.recordIssued(secondRequest) == LedgerIssueStatus::Recorded);
  resolved = ledger.resolve(
      secondRequest,
      CompletionRecord(secondRequest.identity, DownstreamOutcome::Applied),
      ready);
  CHECK(resolved.status == LedgerResolveStatus::Resolved);
  CHECK(resolved.summary.outcome == WorkCompletionOutcome::Applied);
  CHECK(resolved.summary.settled);
  CHECK(resolved.summary.drained);
  CHECK(resolved.decision.notice == CompletionNotice::None);
  CHECK(ledger.release(work2.identity) == LedgerReleaseStatus::Released);
  CHECK(ledger.empty());
}

void testCompletionLedgerStaleRetryAndFailureEdges() {
  EndpointRoute route = makeHoldingRoute(8U, 0U, 20U, 2U);
  const RouteTableView routes(&route, 1U);
  CHECK(routes.validate() == RouteTableStatus::Valid);
  const ForwardPlanner planner(routes, ForwardPlanOptions(8U, 1U));
  const SessionStateView generation3(
      true, 3U, 3U, SessionPhase::Ready);

  CompletionLedgerSlot slots[2];
  CompletionLedger ledger(slots, 2U);
  uint16_t values[] = {11U, 12U};
  HoldingIngressWorkView work = makeHoldingIngressWork(
      WorkIdentity(65535U, 3U), 0U, 2U,
      IngressDelivery::Tracked, values);
  HoldingForwardCursor cursor;
  CHECK(planner.begin(work, cursor) == ForwardPlanStatus::Ready);
  PlannedWriteRequest first;
  PlannedWriteRequest final;
  CHECK(planner.next(cursor, 31U, first) == ForwardNextStatus::Planned);
  CHECK(planner.next(cursor, 32U, final) == ForwardNextStatus::Planned);
  CHECK(ledger.reserve(work.identity, work.delivery) ==
        LedgerReserveStatus::Reserved);
  CHECK(!ledger.closePlanningAfterFailure(work.identity));
  CHECK(ledger.recordIssued(first) == LedgerIssueStatus::Recorded);

  CompletionAggregate directAggregate(work.identity, work.delivery);
  const CompletionDecision invalidDirect = resolveCompletion(
      first,
      CompletionRecord(first.identity,
          static_cast<DownstreamOutcome>(0xFFU)),
      generation3,
      directAggregate);
  CHECK(invalidDirect.status == CompletionDecisionStatus::InvalidOutcome);
  CHECK(!directAggregate.failed);
  CHECK(!directAggregate.complete);
  CHECK(!directAggregate.noticeEmitted);
  CHECK(directAggregate.nextFragmentIndex == 0U);

  PlannedWriteRequest fabricated = first;
  fabricated.downstreamStart = static_cast<uint16_t>(
      fabricated.downstreamStart + 1U);
  LedgerResolution resolution = ledger.resolve(
      fabricated,
      CompletionRecord(fabricated.identity, DownstreamOutcome::Applied),
      generation3);
  CHECK(resolution.status == LedgerResolveStatus::RequestMismatch);
  CHECK(resolution.summary.outstandingRequests == 1U);

  resolution = ledger.resolve(
      first,
      CompletionRecord(first.identity, DownstreamOutcome::Applied),
      SessionStateView(true, 4U, 4U, SessionPhase::Ready));
  CHECK(resolution.status == LedgerResolveStatus::CompletionRejected);
  CHECK(resolution.decision.status == CompletionDecisionStatus::StaleSession);
  CHECK(resolution.summary.outstandingRequests == 1U);

  resolution = ledger.resolve(
      first,
      CompletionRecord(first.identity,
          static_cast<DownstreamOutcome>(0xFFU)),
      generation3);
  CHECK(resolution.status == LedgerResolveStatus::InvalidOutcome);
  CHECK(resolution.summary.outstandingRequests == 1U);

  // Retry attempts are deliberately invisible to the ledger. Only the final
  // outcome is resolved, so one issued request creates one completion debt.
  resolution = ledger.resolve(
      first,
      CompletionRecord(first.identity, DownstreamOutcome::SendUncertain),
      generation3);
  CHECK(resolution.status == LedgerResolveStatus::Resolved);
  CHECK(resolution.summary.outstandingRequests == 0U);
  CHECK(resolution.summary.settled);
  CHECK(!resolution.summary.drained);  // planning is still open
  CHECK(ledger.closePlanningAfterFailure(work.identity));
  CHECK(ledger.summary(work.identity, resolution.summary));
  CHECK(resolution.summary.drained);
  CHECK(resolution.summary.outcome == WorkCompletionOutcome::UncertainSend);
  CHECK(ledger.release(work.identity) == LedgerReleaseStatus::Released);

  // A successful non-final fragment cannot be misreported as complete by
  // closing planning. Cancellation without a failure uses abandon().
  HoldingIngressWorkView partial = work;
  partial.identity = WorkIdentity(100U, 3U);
  HoldingForwardCursor partialCursor;
  CHECK(planner.begin(partial, partialCursor) == ForwardPlanStatus::Ready);
  PlannedWriteRequest partialRequest;
  CHECK(planner.next(partialCursor, 40U, partialRequest) ==
        ForwardNextStatus::Planned);
  CHECK(!partialRequest.finalFragment);
  CHECK(ledger.reserve(partial.identity, partial.delivery) ==
        LedgerReserveStatus::Reserved);
  CHECK(ledger.recordIssued(partialRequest) == LedgerIssueStatus::Recorded);
  resolution = ledger.resolve(
      partialRequest,
      CompletionRecord(partialRequest.identity, DownstreamOutcome::Applied),
      generation3);
  CHECK(resolution.status == LedgerResolveStatus::Resolved);
  CHECK(!ledger.closePlanningAfterFailure(partial.identity));
  CHECK(ledger.summary(partial.identity, resolution.summary));
  CHECK(!resolution.summary.settled);
  CHECK(!resolution.summary.drained);
  CHECK(resolution.summary.outcome == WorkCompletionOutcome::Pending);
  CHECK(ledger.abandon(partial.identity));

  // The same wrapping token in a new generation is a distinct work identity.
  work.identity = WorkIdentity(65535U, 4U);
  CHECK(ledger.reserve(work.identity, work.delivery) ==
        LedgerReserveStatus::Reserved);
  CHECK(ledger.abandon(work.identity));
  CHECK(!ledger.abandon(work.identity));

  CHECK(ledger.reserve(WorkIdentity(7U, 3U), IngressDelivery::Tracked) ==
        LedgerReserveStatus::Reserved);
  CHECK(ledger.reserve(WorkIdentity(8U, 4U), IngressDelivery::Tracked) ==
        LedgerReserveStatus::Reserved);
  CHECK(ledger.abandonNotCurrent(
            SessionStateView(true, 4U, 4U, SessionPhase::Ready)) == 1U);
  CHECK(ledger.size() == 1U);
  CHECK(ledger.abandonGeneration(4U) == 1U);
  CHECK(ledger.empty());

  CompletionLedger invalid(nullptr, 1U);
  CHECK(!invalid.validStorage());
  CHECK(invalid.reserve(WorkIdentity(1U, 1U), IngressDelivery::Tracked) ==
        LedgerReserveStatus::InvalidStorage);
}

void testPollPlannerAdmissionFairnessAndWrap() {
  uint16_t buffers[3][2] = {};
  PollCandidate polls[3];
  for(uint16_t index = 0U; index < 3U; ++index){
    polls[index] = PollCandidate(
        makeHoldingPoll(static_cast<uint8_t>(index + 1U),
                        0U, buffers[index], 2U),
        100U,
        true);
  }

  PollPlanner planner(PollPlannerOptions(2U));
  PollPlannerState state;
  PollSelection selection;

  PollPlannerState scanState;
  PollScanCursor scan;
  planner.beginPollScan(3U, scanState, scan);
  CHECK(scan.active);
  uint16_t scannedIndex = 0U;
  CHECK(planner.nextIndex(scan, scannedIndex) ==
        PollScanIndexStatus::Candidate);
  CHECK(scannedIndex == 0U);
  // Application policy declines child zero for this handler pass. Advancing
  // only the transient scan must find child one without consuming global
  // cursor/lastPoll state.
  CHECK(planner.nextIndex(scan, scannedIndex) ==
        PollScanIndexStatus::Candidate);
  CHECK(scannedIndex == 1U);
  CHECK(scanState.nextPollIndex == 0U);
  CHECK(!scanState.hasConsumedPoll);
  CHECK(planner.commit(
            PollSelection(ScheduledActionKind::Poll, scannedIndex),
            3U, 100U, scanState));
  CHECK(scanState.nextPollIndex == 2U);
  CHECK(scanState.lastPollAt == 100U);

  planner.beginPollScan(3U, state, scan);
  CHECK(planner.nextDue(100U, polls, 3U, scan, selection) ==
        PollDueStatus::Due);
  CHECK(selection.pollIndex == 0U);
  CHECK(selection.pollCountBound);
  CHECK(selection.originatingPollCount == 3U);
  PollPlannerState changedCountState;
  CHECK(!planner.commit(selection, 2U, 100U, changedCountState));
  CHECK(!changedCountState.hasConsumedPoll);
  CHECK(changedCountState.nextPollIndex == 0U);
  CHECK(planner.nextDue(100U, polls, 2U, scan, selection) ==
        PollDueStatus::CandidateCountChanged);

  CHECK(planner.select(100U, true, polls, 3U, state, selection) ==
        PollSelectStatus::Selected);
  CHECK(selection.kind == ScheduledActionKind::ForwardWrite);
  CHECK(selection.pollCountBound);
  CHECK(selection.originatingPollCount == 3U);
  CHECK(state.forwardsSincePoll == 0U);
  CHECK(state.nextPollIndex == 0U);

  // A planner-produced selection cannot be committed against a resized
  // candidate set. The state stays untouched. Manually constructed selections
  // remain unbound for applications that own their own selection policy.
  CHECK(!planner.commit(selection, 2U, 100U, state));
  CHECK(state.forwardsSincePoll == 0U);
  CHECK(state.nextPollIndex == 0U);
  PollPlannerState manualState;
  CHECK(planner.commit(
      PollSelection(ScheduledActionKind::ForwardWrite, 0U),
      9U, 100U, manualState));
  CHECK(manualState.forwardsSincePoll == 1U);

  // Rejected queue admission does not consume the proposal.
  PollSelection repeated;
  CHECK(planner.select(100U, true, polls, 3U, state, repeated) ==
        PollSelectStatus::Selected);
  CHECK(repeated.kind == selection.kind);
  CHECK(planner.commit(selection, 3U, 100U, state));
  CHECK(state.forwardsSincePoll == 1U);

  CHECK(planner.select(100U, true, polls, 3U, state, selection) ==
        PollSelectStatus::Selected);
  CHECK(selection.kind == ScheduledActionKind::ForwardWrite);
  CHECK(planner.commit(selection, 3U, 100U, state));
  CHECK(state.forwardsSincePoll == 2U);

  CHECK(planner.select(100U, true, polls, 3U, state, selection) ==
        PollSelectStatus::Selected);
  CHECK(selection.kind == ScheduledActionKind::Poll);
  CHECK(selection.pollIndex == 0U);
  CHECK(planner.commit(selection, 3U, 100U, state));
  CHECK(state.nextPollIndex == 1U);
  CHECK(state.forwardsSincePoll == 0U);
  CHECK(state.hasConsumedPoll);
  CHECK(state.lastPollAt == 100U);

  // No global one-poll-per-tick gate: after application state is refreshed,
  // another handler phase may select and consume another poll at the same now.
  polls[0].enabled = false;
  CHECK(planner.select(100U, false, polls, 3U, state, selection) ==
        PollSelectStatus::Selected);
  CHECK(selection.kind == ScheduledActionKind::Poll);
  CHECK(selection.pollIndex == 1U);
  CHECK(planner.commit(selection, 3U, 100U, state));
  CHECK(state.nextPollIndex == 2U);
  CHECK(state.lastPollAt == 100U);

  polls[1].enabled = false;
  CHECK(planner.select(100U, false, polls, 3U, state, selection) ==
        PollSelectStatus::Selected);
  CHECK(selection.pollIndex == 2U);
  CHECK(planner.commit(selection, 3U, 101U, state));
  CHECK(state.nextPollIndex == 0U);  // round-robin wraps
  CHECK(state.lastPollAt == 101U);

  polls[2].enabled = false;
  CHECK(planner.select(100U, false, polls, 3U, state, selection) ==
        PollSelectStatus::NoAction);
  CHECK(planner.select(100U, false, nullptr, 1U, state, selection) ==
        PollSelectStatus::InvalidStorage);
  CHECK(!planner.commit(PollSelection(), 3U, 100U, state));
  CHECK(!planner.commit(PollSelection(ScheduledActionKind::Poll, 3U),
                        3U, 100U, state));

  CHECK(pollDeadlineReached32(4U, 0xFFFFFFF0UL));
  CHECK(!pollDeadlineReached32(0xFFFFFFF0UL, 4U));
}

void testPollIndexScanPublishesOnlyConsumedState() {
  PollIndexScan abandoned(3U, 1U, 77U);
  uint16_t index = 0xFFFFU;
  CHECK(abandoned.next(index));
  CHECK(index == 1U);
  CHECK(abandoned.next(index));
  CHECK(index == 2U);
  CHECK(abandoned.next(index));
  CHECK(index == 0U);
  CHECK(!abandoned.next(index));
  CHECK(abandoned.nextPollIndex() == 1U);
  CHECK(abandoned.lastPollAt() == 77U);

  PollIndexScan invalidIndex(3U, 1U, 77U);
  CHECK(!invalidIndex.commitConsumed(3U, 3U, 99U));
  CHECK(invalidIndex.nextPollIndex() == 1U);
  CHECK(invalidIndex.lastPollAt() == 77U);

  PollIndexScan changedCount(3U, 1U, 77U);
  CHECK(changedCount.next(index));
  CHECK(index == 1U);
  CHECK(!changedCount.commitConsumed(index, 2U, 99U));
  CHECK(changedCount.nextPollIndex() == 1U);
  CHECK(changedCount.lastPollAt() == 77U);

  PollIndexScan consumed(3U, 1U, 77U);
  CHECK(consumed.next(index));
  CHECK(index == 1U);
  CHECK(consumed.next(index));
  CHECK(index == 2U);
  CHECK(consumed.commitConsumed(index, 3U, 99U));
  CHECK(consumed.nextPollIndex() == 0U);
  CHECK(consumed.lastPollAt() == 99U);

  PollIndexScan normalized(3U, 9U, 88U);
  CHECK(normalized.nextPollIndex() == 0U);
  CHECK(normalized.lastPollAt() == 88U);
  CHECK(normalized.next(index));
  CHECK(index == 0U);

  PollIndexScan empty(0U, 9U, 55U);
  CHECK(empty.nextPollIndex() == 0U);
  CHECK(empty.lastPollAt() == 55U);
  CHECK(!empty.next(index));
  CHECK(!empty.commitConsumed(0U, 0U, 66U));
  CHECK(empty.nextPollIndex() == 0U);
  CHECK(empty.lastPollAt() == 55U);
}

void testFacadeTwoPhaseFlowOrderingAndSessionSafety() {
  EndpointRoute routesStorage[2];
  routesStorage[0] = makeHoldingRoute(10U, 100U, 0U, 2U);
  routesStorage[1] = makeHoldingRoute(11U, 102U, 20U, 2U);
  const RouteTableView routes(routesStorage, 2U);
  CHECK(routes.validate() == RouteTableStatus::Valid);
  const ForwardPlanner planner(
      routes,
      ForwardPlanOptions(8U, 2U,
                         ForwardSpanPolicy::PreflightAndSplit,
                         ForwardSpanPolicy::PreflightAndSplit));

  StoreForwardWorkSlot workSlots[2];
  CompletionLedgerSlot ledgerSlots[2];
  CompletionLedger ledger(ledgerSlots, 2U);
  StoreForwardBridge bridge(
      planner, workSlots, 2U, ledger,
      PollPlanner(PollPlannerOptions(1U)));
  CHECK(bridge.validStorage());

  const SessionStateView ready(true, 9U, 9U, SessionPhase::Ready);
  uint16_t firstValues[] = {1U, 2U, 3U, 4U};
  uint16_t secondValues[] = {8U};
  const HoldingIngressWorkView firstWork = makeHoldingIngressWork(
      WorkIdentity(1U, 9U), 100U, 4U,
      IngressDelivery::LatestState, firstValues);
  const HoldingIngressWorkView secondWork = makeHoldingIngressWork(
      WorkIdentity(2U, 9U), 100U, 1U,
      IngressDelivery::Tracked, secondValues);
  int firstContext = 17;
  CHECK(bridge.admitWork(firstWork, ready, &firstContext).status ==
        StoreForwardAdmitStatus::Admitted);
  CHECK(bridge.admitWork(secondWork, ready).status ==
        StoreForwardAdmitStatus::Admitted);
  CHECK(bridge.admitWork(makeHoldingIngressWork(
            WorkIdentity(3U, 9U), 100U, 1U,
            IngressDelivery::Tracked, secondValues), ready).status ==
        StoreForwardAdmitStatus::QueueFull);

  uint16_t pollValues[2] = {};
  PollCandidate poll(
      makeHoldingPoll(10U, 0U, pollValues, 2U), 50U, true, true);
  StoreForwardAction preview1;
  CHECK(bridge.nextAction(ready, 50U, &poll, 1U, preview1) ==
        StoreForwardNextStatus::Ready);
  CHECK(preview1.kind == ScheduledActionKind::ForwardWrite);
  CHECK(preview1.write.endpointId == 10U);
  CHECK(preview1.write.sourceOffset == 0U);
  CHECK(preview1.sequence == 1UL);
  CHECK(preview1.workContext == &firstContext);

  // Preview is idempotent and creates neither debt nor fairness movement.
  StoreForwardAction previewAgain;
  CHECK(bridge.nextAction(ready, 50U, &poll, 1U, previewAgain) ==
        StoreForwardNextStatus::Ready);
  CHECK(samePlannedWriteRequest(preview1.write, previewAgain.write));
  CHECK(previewAgain.sequence == 1UL);
  CHECK(bridge.pollState().forwardsSincePoll == 0U);
  WorkCompletionSummary summary;
  CHECK(ledger.summary(firstWork.identity, summary));
  CHECK(summary.outstandingRequests == 0U);

  StoreForwardAction corrupt = preview1;
  corrupt.scheduling.kind = ScheduledActionKind::Poll;
  CHECK(bridge.admitAction(corrupt, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::StaleProposal);
  corrupt = preview1;
  ++corrupt.write.endpointId;
  CHECK(bridge.admitAction(corrupt, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::StaleProposal);
  corrupt = preview1;
  corrupt.scheduling.pollCountBound = false;
  CHECK(bridge.admitAction(corrupt, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::StaleProposal);
  CHECK(bridge.pollState().forwardsSincePoll == 0U);
  CHECK(ledger.summary(firstWork.identity, summary));
  CHECK(summary.outstandingRequests == 0U);

  CHECK(bridge.admitAction(preview1, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::Admitted);
  CHECK(bridge.actionInFlight());
  CHECK(bridge.pollState().forwardsSincePoll == 1U);
  CHECK(ledger.summary(firstWork.identity, summary));
  CHECK(summary.outstandingRequests == 1U);
  CHECK(bridge.admitAction(previewAgain, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::ActionInFlight);
  StoreForwardAction unavailable;
  CHECK(bridge.nextAction(ready, 50U, &poll, 1U, unavailable) ==
        StoreForwardNextStatus::ActionInFlight);

  StoreForwardAction wrong = preview1;
  ++wrong.sequence;
  CHECK(bridge.complete(wrong, DownstreamOutcome::Applied, ready).status ==
        StoreForwardCompleteStatus::ActionMismatch);
  wrong = preview1;
  ++wrong.downstream.endpointId;
  CHECK(bridge.complete(wrong, DownstreamOutcome::Applied, ready).status ==
        StoreForwardCompleteStatus::ActionMismatch);
  wrong = preview1;
  wrong.downstream.consistencyContext = &firstContext;
  CHECK(bridge.complete(wrong, DownstreamOutcome::Applied, ready).status ==
        StoreForwardCompleteStatus::ActionMismatch);
  wrong = preview1;
  wrong.workContext = nullptr;
  CHECK(bridge.complete(wrong, DownstreamOutcome::Applied, ready).status ==
        StoreForwardCompleteStatus::ActionMismatch);
  CHECK(bridge.actionInFlight());
  CHECK(ledger.summary(firstWork.identity, summary));
  CHECK(summary.outstandingRequests == 1U);
  StoreForwardCompletion completed = bridge.complete(
      preview1, DownstreamOutcome::Applied, ready);
  CHECK(completed.status == StoreForwardCompleteStatus::Completed);
  CHECK(completed.decision.appliedAction ==
        AppliedImageAction::MarkAppliedRange);
  CHECK(completed.workContext == &firstContext);
  CHECK(!completed.workRetired);

  // The due poll now wins. A rejected proposal still leaves the cursor and
  // lastPoll untouched until admitAction commits it.
  StoreForwardAction pollAction;
  CHECK(bridge.nextAction(ready, 50U, &poll, 1U, pollAction) ==
        StoreForwardNextStatus::Ready);
  CHECK(pollAction.kind == ScheduledActionKind::Poll);
  CHECK(pollAction.sequence == 2UL);
  CHECK(!bridge.pollState().hasConsumedPoll);
  CHECK(bridge.nextAction(ready, 50U, &poll, 1U, unavailable) ==
        StoreForwardNextStatus::Ready);
  CHECK(unavailable.sequence == pollAction.sequence);
  corrupt = pollAction;
  ++corrupt.downstream.endpointId;
  CHECK(bridge.admitAction(corrupt, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::StaleProposal);
  corrupt = pollAction;
  corrupt.scheduling.kind = ScheduledActionKind::ForwardWrite;
  CHECK(bridge.admitAction(corrupt, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::StaleProposal);
  const uint8_t originalPollEndpoint = poll.request.endpointId;
  ++poll.request.endpointId;
  CHECK(bridge.admitAction(pollAction, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::StaleProposal);
  poll.request.endpointId = originalPollEndpoint;
  CHECK(!bridge.pollState().hasConsumedPoll);
  CHECK(bridge.admitAction(pollAction, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::Admitted);
  CHECK(bridge.pollState().hasConsumedPoll);
  CHECK(bridge.pollState().lastPollAt == 50U);
  wrong = pollAction;
  ++wrong.downstream.endpointId;
  CHECK(bridge.complete(wrong, DownstreamOutcome::Applied, ready).status ==
        StoreForwardCompleteStatus::ActionMismatch);
  wrong = pollAction;
  wrong.downstream.registerBuffer = firstValues;
  CHECK(bridge.complete(wrong, DownstreamOutcome::Applied, ready).status ==
        StoreForwardCompleteStatus::ActionMismatch);
  wrong = pollAction;
  wrong.downstream.consistencyContext = &firstContext;
  CHECK(bridge.complete(wrong, DownstreamOutcome::Applied, ready).status ==
        StoreForwardCompleteStatus::ActionMismatch);
  CHECK(bridge.actionInFlight());
  completed = bridge.complete(
      pollAction, DownstreamOutcome::SendUncertain, ready);
  CHECK(completed.status == StoreForwardCompleteStatus::Completed);
  CHECK(completed.kind == ScheduledActionKind::Poll);
  CHECK(!completed.workRetired);

  // Disable the already-consumed poll phase, then finish the second fragment.
  poll.enabled = false;
  StoreForwardAction finalWrite;
  CHECK(bridge.nextAction(ready, 51U, &poll, 1U, finalWrite) ==
        StoreForwardNextStatus::Ready);
  CHECK(finalWrite.write.endpointId == 11U);
  CHECK(finalWrite.write.sourceOffset == 2U);
  CHECK(finalWrite.write.finalFragment);
  CHECK(bridge.admitAction(finalWrite, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::Admitted);
  completed = bridge.complete(
      finalWrite, DownstreamOutcome::Applied, ready);
  CHECK(completed.status == StoreForwardCompleteStatus::Completed);
  CHECK(completed.work.outcome == WorkCompletionOutcome::Applied);
  CHECK(completed.work.drained);
  CHECK(completed.workRetired);
  CHECK(bridge.queuedWork() == 1U);

  // A failure settles and retires a non-final work without emitting later
  // unissued fragments. No desired-cache rollback occurs in the facade.
  StoreForwardAction failed;
  CHECK(bridge.nextAction(ready, 52U, &poll, 1U, failed) ==
        StoreForwardNextStatus::Ready);
  CHECK(bridge.admitAction(failed, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::Admitted);
  completed = bridge.complete(
      failed, DownstreamOutcome::DefinitelyNotSent, ready);
  CHECK(completed.status == StoreForwardCompleteStatus::Completed);
  CHECK(completed.work.outcome ==
        WorkCompletionOutcome::DefinitelyNotSent);
  CHECK(completed.work.settled);
  CHECK(completed.work.drained);
  CHECK(completed.workRetired);
  CHECK(completed.decision.notice == CompletionNotice::WorkFailed);
  CHECK(bridge.queuedWork() == 0U);

  // A consumed, policy-gated poll phase advances fairness/lastPoll without a
  // wire request, in-flight state, or request-sequence change.
  poll.enabled = true;
  poll.requiresDispatch = false;
  poll.dueAt = 53U;
  StoreForwardAction consumedPoll;
  const uint32_t beforeSequence = bridge.lastRequestSequence();
  CHECK(bridge.nextAction(ready, 53U, &poll, 1U, consumedPoll) ==
        StoreForwardNextStatus::Ready);
  CHECK(consumedPoll.kind == ScheduledActionKind::Poll);
  CHECK(!consumedPoll.requiresCompletion);
  CHECK(consumedPoll.sequence == 0UL);
  corrupt = consumedPoll;
  corrupt.scheduling.kind = ScheduledActionKind::ForwardWrite;
  CHECK(bridge.admitAction(corrupt, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::StaleProposal);
  CHECK(bridge.pollState().lastPollAt == 50U);
  CHECK(bridge.admitAction(consumedPoll, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::Consumed);
  CHECK(!bridge.actionInFlight());
  CHECK(bridge.lastRequestSequence() == beforeSequence);
  CHECK(bridge.pollState().lastPollAt == 53U);

  // Complete an admitted old-generation request after lifecycle rollover. It
  // is rejected before cache/notice mutation and its stale slot is retired.
  poll.enabled = false;
  const HoldingIngressWorkView staleWork = makeHoldingIngressWork(
      WorkIdentity(3U, 9U), 100U, 1U,
      IngressDelivery::Tracked, secondValues);
  CHECK(bridge.admitWork(staleWork, ready).status ==
        StoreForwardAdmitStatus::Admitted);
  StoreForwardAction staleAction;
  CHECK(bridge.nextAction(ready, 54U, &poll, 1U, staleAction) ==
        StoreForwardNextStatus::Ready);
  CHECK(bridge.admitAction(staleAction, ready, &poll, 1U) ==
        StoreForwardActionAdmitStatus::Admitted);
  completed = bridge.complete(
      staleAction,
      DownstreamOutcome::Applied,
      SessionStateView(true, 10U, 10U, SessionPhase::Ready));
  CHECK(completed.status == StoreForwardCompleteStatus::StaleSession);
  CHECK(completed.decision.status == CompletionDecisionStatus::StaleSession);
  CHECK(completed.decision.appliedAction == AppliedImageAction::None);
  CHECK(completed.decision.notice == CompletionNotice::None);
  CHECK(completed.workRetired);
  CHECK(completed.work.outstandingRequests == 1U);
  CHECK(!completed.work.drained);
  CHECK(bridge.queuedWork() == 0U);
  CHECK(ledger.empty());
}

void testFacadePreservesCrossTableAdmissionOrder() {
  EndpointRoute route;
  route.endpointId = 6U;
  const uint8_t coilsTable = static_cast<uint8_t>(RegisterTable::Coils);
  const uint8_t holdingTable =
      static_cast<uint8_t>(RegisterTable::HoldingRegisters);
  route.upstream[holdingTable] = AddressRange(20U, 1U);
  route.downstream[holdingTable] = AddressRange(2U, 1U);
  route.upstream[coilsTable] = AddressRange(40U, 1U);
  route.downstream[coilsTable] = AddressRange(7U, 1U);
  const RouteTableView routes(&route, 1U);
  CHECK(routes.validate() == RouteTableStatus::Valid);

  StoreForwardWorkSlot workStorage[2];
  CompletionLedgerSlot ledgerStorage[2];
  CompletionLedger ledger(ledgerStorage, 2U);
  StoreForwardBridge bridge(
      ForwardPlanner(routes, ForwardPlanOptions(1U, 1U)),
      workStorage,
      2U,
      ledger,
      PollPlanner(PollPlannerOptions(0xFFFFU)));
  const SessionStateView ready(true, 12U, 12U, SessionPhase::Ready);
  uint16_t holdingValue = 0x1234U;
  const bool coilValue = true;
  const HoldingIngressWorkView holding = makeHoldingIngressWork(
      WorkIdentity(1U, 12U), 20U, 1U,
      IngressDelivery::Tracked, &holdingValue);
  const CoilIngressWorkView coil = makeCoilIngressWork(
      WorkIdentity(2U, 12U), 40U, 1U,
      IngressDelivery::Tracked, &coilValue);
  int holdingContext = 1;
  int coilContext = 2;
  CHECK(bridge.admitWork(holding, ready, &holdingContext).status ==
        StoreForwardAdmitStatus::Admitted);
  CHECK(bridge.admitWork(coil, ready, &coilContext).status ==
        StoreForwardAdmitStatus::Admitted);

  StoreForwardAction action;
  CHECK(bridge.nextAction(ready, 0U, nullptr, 0U, action) ==
        StoreForwardNextStatus::Ready);
  CHECK(action.write.table == RegisterTable::HoldingRegisters);
  CHECK(action.write.operation ==
        DownstreamOperation::WriteSingleHoldingRegister);
  CHECK(action.sequence == 1UL);
  CHECK(action.workContext == &holdingContext);
  CHECK(bridge.admitAction(action, ready, nullptr, 0U) ==
        StoreForwardActionAdmitStatus::Admitted);
  StoreForwardCompletion completion = bridge.complete(
      action, DownstreamOutcome::Applied, ready);
  CHECK(completion.status == StoreForwardCompleteStatus::Completed);
  CHECK(completion.workRetired);
  CHECK(completion.workContext == &holdingContext);

  CHECK(bridge.nextAction(ready, 0U, nullptr, 0U, action) ==
        StoreForwardNextStatus::Ready);
  CHECK(action.write.table == RegisterTable::Coils);
  CHECK(action.write.operation == DownstreamOperation::WriteSingleCoil);
  CHECK(action.sequence == 2UL);
  CHECK(action.workContext == &coilContext);
  CHECK(bridge.admitAction(action, ready, nullptr, 0U) ==
        StoreForwardActionAdmitStatus::Admitted);
  completion = bridge.complete(action, DownstreamOutcome::Applied, ready);
  CHECK(completion.status == StoreForwardCompleteStatus::Completed);
  CHECK(completion.workRetired);
  CHECK(completion.workContext == &coilContext);
  CHECK(bridge.queuedWork() == 0U);
  CHECK(ledger.empty());
}

void testFacadeStaleQueueAndValidationEdges() {
  EndpointRoute route = makeHoldingRoute(2U, 0U, 0U, 1U);
  const RouteTableView routes(&route, 1U);
  CHECK(routes.validate() == RouteTableStatus::Valid);
  const ForwardPlanner planner(routes, ForwardPlanOptions(1U, 1U));
  StoreForwardWorkSlot storage[2];
  CompletionLedgerSlot ledgerStorage[2];
  CompletionLedger ledger(ledgerStorage, 2U);
  StoreForwardBridge bridge(planner, storage, 2U, ledger);
  uint16_t firstValue = 5U;
  uint16_t secondValue = 6U;
  const SessionStateView generation1(
      true, 1U, 1U, SessionPhase::Ready);
  const SessionStateView generation2(
      true, 2U, 2U, SessionPhase::Ready);
  const HoldingIngressWorkView firstWork = makeHoldingIngressWork(
      WorkIdentity(1U, 1U), 0U, 1U,
      IngressDelivery::Tracked, &firstValue);
  const HoldingIngressWorkView secondWork = makeHoldingIngressWork(
      WorkIdentity(2U, 2U), 0U, 1U,
      IngressDelivery::Tracked, &secondValue);
  int firstContext = 11;
  int secondContext = 22;
  CHECK(bridge.admitWork(firstWork, generation1, &firstContext).status ==
        StoreForwardAdmitStatus::Admitted);
  CHECK(bridge.admitWork(secondWork, generation2, &secondContext).status ==
        StoreForwardAdmitStatus::Admitted);
  StoreForwardAction action;
  CHECK(bridge.nextAction(generation2, 0U, nullptr, 0U, action) ==
        StoreForwardNextStatus::StaleWork);

  StoreForwardRetirement retired;
  CHECK(bridge.discardOneStaleWork(generation1, retired) ==
        StoreForwardRetireStatus::WorkStillCurrent);
  CHECK(retired.userContext == nullptr);
  CHECK(bridge.discardOneStaleWork(generation2, retired) ==
        StoreForwardRetireStatus::Retired);
  CHECK(sameWorkIdentity(retired.work, firstWork.identity));
  CHECK(retired.table == RegisterTable::HoldingRegisters);
  CHECK(retired.userContext == &firstContext);
  CHECK(bridge.queuedWork() == 1U);
  CHECK(ledger.size() == 1U);

  // Current work is not discarded implicitly. Explicit one-at-a-time
  // abandonment returns its context before the caller frees the snapshot.
  CHECK(bridge.discardOneStaleWork(generation2, retired) ==
        StoreForwardRetireStatus::WorkStillCurrent);
  CHECK(bridge.abandonFront(retired) == StoreForwardRetireStatus::Retired);
  CHECK(sameWorkIdentity(retired.work, secondWork.identity));
  CHECK(retired.table == RegisterTable::HoldingRegisters);
  CHECK(retired.userContext == &secondContext);
  CHECK(bridge.abandonFront(retired) == StoreForwardRetireStatus::Empty);
  CHECK(bridge.queuedWork() == 0U);
  CHECK(ledger.empty());

  CHECK(bridge.admitWork(firstWork, generation1, &firstContext).status ==
        StoreForwardAdmitStatus::Admitted);
  CHECK(bridge.nextAction(generation1, 0U, nullptr, 1U, action) ==
        StoreForwardNextStatus::InvalidPollStorage);
  CHECK(bridge.reset());

  StoreForwardBridge invalid(
      planner, nullptr, 1U, ledger);
  CHECK(!invalid.validStorage());
  CHECK(invalid.admitWork(firstWork, generation1).status ==
        StoreForwardAdmitStatus::InvalidStorage);
}

struct FakeMaster {
  enum Result : uint8_t {
    Ok = 0U,
    InvalidQuantity = 2U,
    InvalidBuffer = 3U,
    InvalidOperation = 4U,
  };

  DownstreamOperation called;
  uint8_t endpoint;
  uint16_t start;
  uint16_t count;
  bool* coils;
  uint16_t* registers;
  uint16_t scalar;
  uint8_t exception;

  FakeMaster()
      : called(static_cast<DownstreamOperation>(0xFFU)),
        endpoint(0U),
        start(0U),
        count(0U),
        coils(nullptr),
        registers(nullptr),
        scalar(0U),
        exception(0x42U) {}

  Result note(DownstreamOperation operation,
              uint8_t id,
              uint16_t address,
              uint16_t quantity) {
    called = operation;
    endpoint = id;
    start = address;
    count = quantity;
    return Ok;
  }

  Result readCoils(uint8_t id, uint16_t address, bool* values,
                   uint16_t quantity) {
    coils = values;
    return note(DownstreamOperation::ReadCoils, id, address, quantity);
  }
  Result readDiscreteInputs(uint8_t id, uint16_t address, bool* values,
                            uint16_t quantity) {
    coils = values;
    return note(DownstreamOperation::ReadDiscreteInputs,
                id, address, quantity);
  }
  Result readHoldingRegisters(uint8_t id, uint16_t address, uint16_t* values,
                              uint16_t quantity) {
    registers = values;
    return note(DownstreamOperation::ReadHoldingRegisters,
                id, address, quantity);
  }
  Result readInputRegisters(uint8_t id, uint16_t address, uint16_t* values,
                            uint16_t quantity) {
    registers = values;
    return note(DownstreamOperation::ReadInputRegisters,
                id, address, quantity);
  }
  Result writeSingleCoil(uint8_t id, uint16_t address, bool value) {
    scalar = value ? 1U : 0U;
    return note(DownstreamOperation::WriteSingleCoil, id, address, 1U);
  }
  Result writeSingleHoldingRegister(uint8_t id, uint16_t address,
                                    uint16_t value) {
    scalar = value;
    return note(DownstreamOperation::WriteSingleHoldingRegister,
                id, address, 1U);
  }
  Result writeMultipleCoils(uint8_t id, uint16_t address, bool* values,
                            uint16_t quantity) {
    coils = values;
    return note(DownstreamOperation::WriteMultipleCoils,
                id, address, quantity);
  }
  Result writeMultipleHoldingRegisters(uint8_t id, uint16_t address,
                                       uint16_t* values,
                                       uint16_t quantity) {
    registers = values;
    return note(DownstreamOperation::WriteMultipleHoldingRegisters,
                id, address, quantity);
  }
  uint8_t getExceptionResponse() { return exception; }
};

void testOptionalMasterAdapter() {
  FakeMaster master;
  const ModbusRTUMasterResultCodes<FakeMaster::Result> codes(
      FakeMaster::InvalidQuantity,
      FakeMaster::InvalidBuffer,
      FakeMaster::InvalidOperation);
  ModbusRTUMasterBackend<FakeMaster, FakeMaster::Result> backend(master, codes);

  bool coilValues[] = {true, false};
  uint16_t registerValues[] = {0x1234U, 0x5678U};
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

  // Exercise every conventional master method through the actual generic
  // executor switch. This catches signature/order drift in the thin adapter,
  // not merely whether the adapter class can be instantiated.
  for(uint8_t index = 0U; index < 8U; ++index){
    master = FakeMaster();
    DownstreamRequest request;
    request.sequence = static_cast<uint32_t>(91U + index);
    request.operation = operations[index];
    request.endpointId = 12U;
    request.startAddress = 30U;
    request.quantity = 2U;
    request.coilBuffer = coilValues;
    request.registerBuffer = registerValues;
    request.coilValue = true;
    request.registerValue = 0xBEEFU;

    const DownstreamCompletion<FakeMaster::Result> completion =
        executeDownstreamRequest(backend, request);
    CHECK(completion.sequence == request.sequence);
    CHECK(completion.result == FakeMaster::Ok);
    CHECK(completion.exceptionCode == 0x42U);
    CHECK(master.called == operations[index]);
    CHECK(master.endpoint == 12U);
    CHECK(master.start == 30U);
    const bool single = operations[index] ==
                            DownstreamOperation::WriteSingleCoil ||
                        operations[index] ==
                            DownstreamOperation::WriteSingleHoldingRegister;
    CHECK(master.count == (single ? 1U : 2U));

    switch(operations[index]){
      case DownstreamOperation::ReadCoils:
      case DownstreamOperation::ReadDiscreteInputs:
      case DownstreamOperation::WriteMultipleCoils:
        CHECK(master.coils == coilValues);
        break;
      case DownstreamOperation::ReadHoldingRegisters:
      case DownstreamOperation::ReadInputRegisters:
      case DownstreamOperation::WriteMultipleHoldingRegisters:
        CHECK(master.registers == registerValues);
        break;
      case DownstreamOperation::WriteSingleCoil:
        CHECK(master.scalar == 1U);
        break;
      case DownstreamOperation::WriteSingleHoldingRegister:
        CHECK(master.scalar == 0xBEEFU);
        break;
    }
  }

  DownstreamRequest request;
  request.operation = DownstreamOperation::WriteMultipleHoldingRegisters;
  request.registerBuffer = registerValues;
  request.quantity = 0U;
  CHECK(executeDownstreamRequest(backend, request).result ==
        FakeMaster::InvalidQuantity);
  request.quantity = 1U;
  request.registerBuffer = nullptr;
  CHECK(executeDownstreamRequest(backend, request).result ==
        FakeMaster::InvalidBuffer);
  request.operation = static_cast<DownstreamOperation>(0xFFU);
  CHECK(executeDownstreamRequest(backend, request).result ==
        FakeMaster::InvalidOperation);
}

void testNoAllocationCharacterization() {
  const size_t before = g_allocations;
  EndpointRoute route = makeHoldingRoute(1U, 0U, 0U, 1U);
  RouteTableView routes(&route, 1U);
  ForwardPlanner planner(routes, ForwardPlanOptions(1U, 1U));
  StoreForwardWorkSlot workStorage[1];
  CompletionLedgerSlot ledgerStorage[1];
  CompletionLedger ledger(ledgerStorage, 1U);
  StoreForwardBridge bridge(
      planner, workStorage, 1U, ledger,
      PollPlanner(PollPlannerOptions(0U)));
  uint16_t value = 7U;
  const SessionStateView ready(true, 1U, 1U, SessionPhase::Ready);
  const HoldingIngressWorkView work = makeHoldingIngressWork(
      WorkIdentity(1U, 1U), 0U, 1U,
      IngressDelivery::Tracked, &value);
  CHECK(bridge.admitWork(work, ready).status ==
        StoreForwardAdmitStatus::Admitted);
  StoreForwardAction action;
  CHECK(bridge.nextAction(ready, 0U, nullptr, 0U, action) ==
        StoreForwardNextStatus::Ready);
  CHECK(bridge.admitAction(action, ready, nullptr, 0U) ==
        StoreForwardActionAdmitStatus::Admitted);
  CHECK(bridge.complete(action, DownstreamOutcome::Applied, ready).status ==
        StoreForwardCompleteStatus::Completed);
  PollIndexScan scan(3U, 1U, 7U);
  uint16_t index = 0U;
  CHECK(scan.next(index));
  CHECK(index == 1U);
  CHECK(scan.commitConsumed(index, 3U, 8U));
  CHECK(g_allocations == before);
}

}  // namespace

void* operator new(size_t size) {
  ++g_allocations;
  void* memory = malloc(size);
  if(memory == nullptr){
    abort();
  }
  return memory;
}

void operator delete(void* memory) noexcept {
  free(memory);
}

int main() {
  testFixedStorageCapacityAndWrap();
  testCompletionLedgerCapacityOrderingAndOutcomes();
  testCompletionLedgerStaleRetryAndFailureEdges();
  testPollPlannerAdmissionFairnessAndWrap();
  testPollIndexScanPublishesOnlyConsumedState();
  testFacadeTwoPhaseFlowOrderingAndSessionSafety();
  testFacadePreservesCrossTableAdmissionOrder();
  testFacadeStaleQueueAndValidationEdges();
  testOptionalMasterAdapter();
  testNoAllocationCharacterization();
  printf("ModbusRTUStoreForwardBridge facade checks: %u (allocations=%lu, "
         "work_slot=%luB ledger_slot=%luB action=%luB facade=%luB)\n",
         g_checks,
         static_cast<unsigned long>(g_allocations),
         static_cast<unsigned long>(sizeof(StoreForwardWorkSlot)),
         static_cast<unsigned long>(sizeof(CompletionLedgerSlot)),
         static_cast<unsigned long>(sizeof(StoreForwardAction)),
         static_cast<unsigned long>(sizeof(StoreForwardBridge)));
  return 0;
}
