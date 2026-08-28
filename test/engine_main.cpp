#include <modbus_rtu_bridge/CompletionTransition.h>
#include <modbus_rtu_bridge/ForwardPlan.h>
#include <modbus_rtu_bridge/IngressWork.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <type_traits>

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

static_assert(sizeof(IngressDelivery) == sizeof(uint8_t),
              "delivery must remain a one-byte work field");
static_assert(sizeof(WorkIdentity) == 4U,
              "work identity must remain two 16-bit serials");
static_assert(std::is_trivially_copyable<WorkIdentity>::value,
              "work identity must remain queue-friendly POD");
static_assert(std::is_trivially_copyable<CoilIngressWorkView>::value,
              "ingress views must remain trivially copyable");
static_assert(sizeof(CoilIngressWorkView) <= 32U,
              "ingress view unexpectedly grew");
static_assert(std::is_trivially_copyable<PlannedWriteRequest>::value,
              "planned requests must remain trivially copyable");
static_assert(std::is_trivially_copyable<CompletionAggregate>::value,
              "completion state must remain caller-storage friendly");
static_assert(sizeof(CompletionAggregate) <= 12U,
              "completion aggregate unexpectedly grew");
static_assert(std::is_trivially_copyable<CompletionDecision>::value,
              "completion decisions must remain queue-friendly POD");
static_assert(sizeof(CompletionDecision) <= 24U,
              "completion decision unexpectedly grew");
static_assert(std::is_trivially_destructible<ForwardPlanner>::value,
              "planner must not acquire dynamic ownership");
static_assert(sizeof(HoldingForwardCursor) <= 48U,
              "forward cursor unexpectedly grew");
static_assert(sizeof(PlannedWriteRequest) <= 80U,
              "planned request unexpectedly grew");

uint8_t tableIndex(RegisterTable table) {
  return static_cast<uint8_t>(table);
}

EndpointRoute makeRoute(uint8_t endpoint,
                        RegisterTable table,
                        uint16_t upstreamStart,
                        uint16_t downstreamStart,
                        uint16_t count) {
  EndpointRoute route;
  route.endpointId = endpoint;
  route.upstream[tableIndex(table)] = AddressRange(upstreamStart, count);
  route.downstream[tableIndex(table)] = AddressRange(downstreamStart, count);
  return route;
}

void testIngressDeliveryIdentityAndSession() {
  CHECK(classifyAcceptedIngress(false, true, false) ==
        IngressDelivery::Tracked);
  CHECK(classifyAcceptedIngress(false, false, false) ==
        IngressDelivery::SilentOrdered);
  CHECK(classifyAcceptedIngress(false, true, true) ==
        IngressDelivery::SilentOrdered);
  CHECK(classifyAcceptedIngress(true, true, false) ==
        IngressDelivery::LatestState);

  CHECK(isAccepted(IngressDelivery::Tracked));
  CHECK(!isAccepted(IngressDelivery::Rejected));
  CHECK(isKnownIngressDelivery(IngressDelivery::LatestState));
  const IngressDelivery malformedDelivery =
      static_cast<IngressDelivery>(0xFFU);
  CHECK(!isKnownIngressDelivery(malformedDelivery));
  CHECK(!isAccepted(malformedDelivery));
  CHECK(tracksPublicCompletion(IngressDelivery::Tracked));
  CHECK(!tracksPublicCompletion(IngressDelivery::SilentOrdered));
  CHECK(requiresOrderedDelivery(IngressDelivery::SilentOrdered));
  CHECK(!requiresOrderedDelivery(IngressDelivery::LatestState));
  CHECK(preservesSourceOrder(IngressDelivery::Tracked));
  CHECK(!preservesSourceOrder(IngressDelivery::LatestState));

  CHECK(nextNonZeroSerial16(0U) == 1U);
  CHECK(nextNonZeroSerial16(65535U) == 1U);
  CHECK(nonZeroSerial16Before(65535U, 1U));
  CHECK(!nonZeroSerial16Before(1U, 65535U));
  CHECK(nonZeroSerial16Before(7U, 8U));
  CHECK(!nonZeroSerial16Before(1U, 32769U));
  CHECK(!nonZeroSerial16Before(32769U, 1U));
  CHECK(!nonZeroSerial16Before(0U, 1U));
  CHECK(nonZeroSerial16Before(1U, 0U));

  const WorkIdentity oldWork(65535U, 9U);
  const WorkIdentity newWork(1U, 9U);
  CHECK(workIdentityBefore(oldWork, newWork));
  CHECK(!workIdentityBefore(newWork, oldWork));
  CHECK(!workIdentityBefore(oldWork, WorkIdentity(1U, 10U)));
  CHECK(sameWorkIdentity(oldWork, WorkIdentity(65535U, 9U)));

  const SessionStateView ready(true, 9U, 9U, SessionPhase::Ready);
  CHECK(admissionOpen(ready));
  CHECK(workCurrent(oldWork, ready));
  CHECK(!workCurrent(oldWork,
                     SessionStateView(true, 10U, 10U,
                                      SessionPhase::Ready)));
  CHECK(!workCurrent(oldWork,
                     SessionStateView(false, 9U, 9U,
                                      SessionPhase::Ready)));
  CHECK(!admissionOpen(SessionStateView(
      true, 9U, 8U, SessionPhase::Ready)));
  CHECK(!admissionOpen(SessionStateView(
      true, 9U, 9U, SessionPhase::Initializing)));

  const bool coils[] = {true, false};
  const CoilIngressWorkView valid = makeCoilIngressWork(
      WorkIdentity(1U, 9U), 4U, 2U, IngressDelivery::Tracked, coils);
  CHECK(valid.validAcceptedShape());
  const CoilIngressWorkView identityless = makeCoilIngressWork(
      WorkIdentity(), 4U, 2U, IngressDelivery::Tracked, coils);
  CHECK(identityless.validAcceptedShape());
  CHECK(!identityless.identity.valid());
  CoilIngressWorkView invalid = valid;
  invalid.snapshot = ImmutableSnapshotView<bool>(coils, 1U);
  CHECK(!invalid.validAcceptedShape());
  invalid = valid;
  invalid.delivery = IngressDelivery::Rejected;
  CHECK(!invalid.validAcceptedShape());
  invalid.delivery = malformedDelivery;
  CHECK(!invalid.validAcceptedShape());
}

void testPlannerPreflightsCompleteRangeAtomically() {
  EndpointRoute gapped[2];
  gapped[0] = makeRoute(
      3U, RegisterTable::HoldingRegisters, 100U, 10U, 3U);
  gapped[1] = makeRoute(
      4U, RegisterTable::HoldingRegisters, 104U, 20U, 2U);
  RouteTableView gappedTable(gapped, 2U);
  CHECK(gappedTable.validate() == RouteTableStatus::Valid);

  const uint16_t values[] = {1U, 2U, 3U};
  const HoldingIngressWorkView gappedWork = makeHoldingIngressWork(
      WorkIdentity(7U, 2U), 102U, 3U,
      IngressDelivery::LatestState, values);
  ForwardPlanner gappedPlanner(
      gappedTable, ForwardPlanOptions(8U, 8U));
  HoldingForwardCursor gappedCursor;
  CHECK(gappedPlanner.begin(gappedWork, gappedCursor) ==
        ForwardPlanStatus::Gap);
  CHECK(!gappedCursor.active);
  PlannedWriteRequest request;
  CHECK(gappedPlanner.next(gappedCursor, 1U, request) ==
        ForwardNextStatus::Complete);

  EndpointRoute contiguous[2];
  contiguous[0] = makeRoute(
      3U, RegisterTable::HoldingRegisters, 100U, 10U, 3U);
  contiguous[1] = makeRoute(
      4U, RegisterTable::HoldingRegisters, 103U, 20U, 3U);
  RouteTableView contiguousTable(contiguous, 2U);
  CHECK(contiguousTable.validate() == RouteTableStatus::Valid);
  ForwardPlanner planner(contiguousTable, ForwardPlanOptions(8U, 8U));

  const HoldingIngressWorkView ordered = makeHoldingIngressWork(
      WorkIdentity(8U, 2U), 102U, 3U,
      IngressDelivery::Tracked, values);
  HoldingForwardCursor orderedCursor;
  CHECK(planner.begin(ordered, orderedCursor) ==
        ForwardPlanStatus::MultipleEndpointsNotAllowed);
  CHECK(!orderedCursor.active);

  ForwardPlanner orderedSplit(
      contiguousTable,
      ForwardPlanOptions(
          8U, 8U,
          ForwardSpanPolicy::PreflightAndSplit,
          ForwardSpanPolicy::PreflightAndSplit));
  CHECK(orderedSplit.begin(ordered, orderedCursor) ==
        ForwardPlanStatus::Ready);

  const HoldingIngressWorkView latest = makeHoldingIngressWork(
      WorkIdentity(9U, 2U), 102U, 3U,
      IngressDelivery::LatestState, values);
  HoldingForwardCursor latestCursor;
  CHECK(planner.begin(latest, latestCursor) == ForwardPlanStatus::Ready);

  ForwardPlanner latestSingle(
      contiguousTable,
      ForwardPlanOptions(
          8U, 8U,
          ForwardSpanPolicy::SingleEndpoint,
          ForwardSpanPolicy::SingleEndpoint));
  CHECK(latestSingle.begin(latest, latestCursor) ==
        ForwardPlanStatus::MultipleEndpointsNotAllowed);

  HoldingIngressWorkView identityless = latest;
  identityless.identity = WorkIdentity();
  CHECK(planner.begin(identityless, latestCursor) == ForwardPlanStatus::Ready);
  PlannedWriteRequest identitylessRequest;
  CHECK(planner.next(latestCursor, identitylessRequest) ==
        ForwardNextStatus::Planned);
  CHECK(!identitylessRequest.identity.valid());
  CHECK(identitylessRequest.identity.requestSequence == 0UL);
  CHECK(planner.begin(
            identityless,
            SessionStateView(true, 2U, 2U, SessionPhase::Ready),
            latestCursor) == ForwardPlanStatus::StaleSession);

  HoldingForwardCursor staleCursor;
  CHECK(planner.begin(
            latest,
            SessionStateView(true, 3U, 3U, SessionPhase::Ready),
            staleCursor) == ForwardPlanStatus::StaleSession);
  CHECK(!staleCursor.active);
  CHECK(planner.begin(
            latest,
            SessionStateView(false, 2U, 2U, SessionPhase::Ready),
            staleCursor) == ForwardPlanStatus::StaleSession);
  CHECK(!staleCursor.active);

  HoldingIngressWorkView rejected = latest;
  rejected.delivery = IngressDelivery::Rejected;
  CHECK(planner.begin(rejected, staleCursor) ==
        ForwardPlanStatus::NotAccepted);
  rejected.delivery = static_cast<IngressDelivery>(0xFFU);
  CHECK(planner.begin(rejected, staleCursor) ==
        ForwardPlanStatus::NotAccepted);

  const ForwardSpanPolicy malformedPolicy =
      static_cast<ForwardSpanPolicy>(0xFFU);
  CHECK(!isKnownForwardSpanPolicy(malformedPolicy));
  ForwardPlanner invalidOrderedPolicy(
      contiguousTable,
      ForwardPlanOptions(
          8U, 8U,
          malformedPolicy,
          ForwardSpanPolicy::PreflightAndSplit));
  CHECK(invalidOrderedPolicy.begin(ordered, staleCursor) ==
        ForwardPlanStatus::InvalidPolicy);
  ForwardPlanner invalidLatestPolicy(
      contiguousTable,
      ForwardPlanOptions(
          8U, 8U,
          ForwardSpanPolicy::SingleEndpoint,
          malformedPolicy));
  CHECK(invalidLatestPolicy.begin(latest, staleCursor) ==
        ForwardPlanStatus::InvalidPolicy);

  ForwardPlanner invalidLimits(contiguousTable, ForwardPlanOptions());
  CHECK(invalidLimits.begin(latest, staleCursor) ==
        ForwardPlanStatus::InvalidLimits);

  const HoldingIngressWorkView empty(
      WorkIdentity(), 100U, 0U, IngressDelivery::LatestState,
      ImmutableSnapshotView<uint16_t>());
  CHECK(planner.begin(empty, staleCursor) == ForwardPlanStatus::Empty);

  const uint16_t overflowValues[] = {4U, 5U};
  const HoldingIngressWorkView overflow = makeHoldingIngressWork(
      WorkIdentity(10U, 2U), 65535U, 2U,
      IngressDelivery::LatestState, overflowValues);
  CHECK(planner.begin(overflow, staleCursor) ==
        ForwardPlanStatus::AddressOverflow);
}

void testPlannerChunksAndPreservesSourceOffsets() {
  EndpointRoute routes[2];
  routes[0] = makeRoute(
      11U, RegisterTable::HoldingRegisters, 100U, 10U, 3U);
  routes[1] = makeRoute(
      12U, RegisterTable::HoldingRegisters, 103U, 30U, 3U);
  RouteTableView table(routes, 2U);
  CHECK(table.validate() == RouteTableStatus::Valid);

  const uint16_t values[] = {
      0x1000U, 0x1001U, 0x1002U, 0x1003U, 0x1004U, 0x1005U};
  const HoldingIngressWorkView work = makeHoldingIngressWork(
      WorkIdentity(65535U, 5U), 100U, 6U,
      IngressDelivery::LatestState, values);
  ForwardPlanner planner(table, ForwardPlanOptions(2U, 2U));
  HoldingForwardCursor cursor;
  CHECK(planner.begin(
            work,
            SessionStateView(true, 5U, 5U, SessionPhase::Ready),
            cursor) == ForwardPlanStatus::Ready);

  const uint8_t endpoints[] = {11U, 11U, 12U, 12U};
  const uint16_t upstreamStarts[] = {100U, 102U, 103U, 105U};
  const uint16_t downstreamStarts[] = {10U, 12U, 30U, 32U};
  const uint16_t quantities[] = {2U, 1U, 2U, 1U};
  const uint16_t offsets[] = {0U, 2U, 3U, 5U};
  uint32_t sequence = 0xFFFFFFFEUL;
  for(uint16_t index = 0U; index < 4U; ++index){
    sequence = nextNonZeroSequence32(sequence);
    PlannedWriteRequest request;
    CHECK(planner.next(cursor, sequence, request) ==
          ForwardNextStatus::Planned);
    CHECK(request.identity.requestSequence == sequence);
    CHECK(request.identity.fragmentIndex == index);
    CHECK(sameWorkIdentity(request.identity.work, work.identity));
    CHECK(request.endpointId == endpoints[index]);
    CHECK(request.upstreamStart == upstreamStarts[index]);
    CHECK(request.downstreamStart == downstreamStarts[index]);
    CHECK(request.quantity == quantities[index]);
    CHECK(request.sourceOffset == offsets[index]);
    CHECK(request.holdingValues == values + offsets[index]);
    CHECK(request.holdingValue == values[offsets[index]]);
    CHECK(request.operation ==
          (quantities[index] == 1U
               ? DownstreamOperation::WriteSingleHoldingRegister
               : DownstreamOperation::WriteMultipleHoldingRegisters));
    CHECK(request.finalFragment == (index == 3U));
  }
  CHECK(sequence == 3UL);
  PlannedWriteRequest complete;
  CHECK(planner.next(cursor, nextNonZeroSequence32(sequence), complete) ==
        ForwardNextStatus::Complete);

  EndpointRoute coilRoute = makeRoute(
      8U, RegisterTable::Coils, 5U, 40U, 3U);
  RouteTableView coilTable(&coilRoute, 1U);
  CHECK(coilTable.validate() == RouteTableStatus::Valid);
  const bool coils[] = {true, false, true};
  const CoilIngressWorkView coilWork = makeCoilIngressWork(
      WorkIdentity(3U, 5U), 5U, 3U,
      IngressDelivery::Tracked, coils);
  ForwardPlanner coilPlanner(coilTable, ForwardPlanOptions(2U, 2U));
  CoilForwardCursor coilCursor;
  CHECK(coilPlanner.begin(coilWork, coilCursor) == ForwardPlanStatus::Ready);
  PlannedWriteRequest first;
  CHECK(coilPlanner.next(coilCursor, 9U, first) ==
        ForwardNextStatus::Planned);
  CHECK(first.operation == DownstreamOperation::WriteMultipleCoils);
  CHECK(first.coilValues == coils);
  CHECK(first.quantity == 2U);
  PlannedWriteRequest second;
  CHECK(coilPlanner.next(coilCursor, 10U, second) ==
        ForwardNextStatus::Planned);
  CHECK(second.operation == DownstreamOperation::WriteSingleCoil);
  CHECK(second.coilValues == coils + 2U);
  CHECK(second.coilValue);
  CHECK(second.sourceOffset == 2U);
  CHECK(second.finalFragment);

  CoilForwardCursor badSequenceCursor;
  CHECK(coilPlanner.begin(coilWork, badSequenceCursor) ==
        ForwardPlanStatus::Ready);
  CHECK(coilPlanner.next(badSequenceCursor, 0U, first) ==
        ForwardNextStatus::InvalidSequence);
  CHECK(coilPlanner.next(badSequenceCursor, first) ==
        ForwardNextStatus::InvalidSequence);
  CHECK(badSequenceCursor.active);
}

void testCompletionDecisionsAndCacheTransitions() {
  CHECK(!CompletionDecision().current());

  EndpointRoute route = makeRoute(
      21U, RegisterTable::HoldingRegisters, 0U, 50U, 4U);
  RouteTableView table(&route, 1U);
  CHECK(table.validate() == RouteTableStatus::Valid);
  const uint16_t desired[] = {10U, 11U, 12U, 13U};
  ForwardPlanner planner(table, ForwardPlanOptions(2U, 2U));
  const HoldingIngressWorkView tracked = makeHoldingIngressWork(
      WorkIdentity(31U, 7U), 0U, 4U,
      IngressDelivery::Tracked, desired);
  HoldingForwardCursor cursor;
  CHECK(planner.begin(tracked, cursor) == ForwardPlanStatus::Ready);
  PlannedWriteRequest first;
  PlannedWriteRequest final;
  CHECK(planner.next(cursor, 101U, first) == ForwardNextStatus::Planned);
  CHECK(planner.next(cursor, 102U, final) == ForwardNextStatus::Planned);

  uint16_t visible[] = {10U, 11U, 12U, 13U, 99U};
  uint16_t applied[] = {1U, 2U, 3U, 4U, 88U};
  const DesiredAppliedCache<uint16_t> cache(
      MutableImageView<uint16_t>(visible, 5U),
      MutableImageView<uint16_t>(applied, 5U));
  const SessionStateView ready(true, 7U, 7U, SessionPhase::Ready);
  CompletionAggregate aggregate(tracked.identity, tracked.delivery);

  CompletionDecision decision = resolveCompletion(
      final,
      CompletionRecord(final.identity, DownstreamOutcome::Applied),
      ready,
      aggregate);
  CHECK(decision.status == CompletionDecisionStatus::OutOfOrder);
  CHECK(decision.appliedAction == AppliedImageAction::None);
  CHECK(decision.notice == CompletionNotice::None);
  CHECK(!aggregate.complete);
  CHECK(aggregate.nextFragmentIndex == 0U);

  CompletionRecord mismatched(first.identity, DownstreamOutcome::Applied);
  mismatched.identity.requestSequence = 999U;
  decision = resolveCompletion(first, mismatched, ready, aggregate);
  CHECK(decision.status == CompletionDecisionStatus::IdentityMismatch);
  CHECK(aggregate.nextFragmentIndex == 0U);

  decision = resolveCompletion(
      first,
      CompletionRecord(first.identity, DownstreamOutcome::Applied),
      SessionStateView(true, 8U, 8U, SessionPhase::Ready),
      aggregate);
  CHECK(decision.status == CompletionDecisionStatus::StaleSession);
  CHECK(aggregate.nextFragmentIndex == 0U);

  decision = resolveCompletion(
      first,
      CompletionRecord(first.identity, DownstreamOutcome::Applied),
      SessionStateView(false, 7U, 7U, SessionPhase::Ready),
      aggregate);
  CHECK(decision.status == CompletionDecisionStatus::StaleSession);
  CHECK(aggregate.nextFragmentIndex == 0U);

  decision = resolveCompletion(
      first,
      CompletionRecord(first.identity, DownstreamOutcome::Applied),
      ready,
      aggregate);
  CHECK(decision.current());
  CHECK(decision.appliedAction == AppliedImageAction::MarkAppliedRange);
  CHECK(decision.desiredDisposition == DesiredImageDisposition::Unchanged);
  CHECK(decision.notice == CompletionNotice::None);
  CHECK(applyAppliedImageTransition(cache, first, decision));
  PlannedWriteRequest unrelated = first;
  unrelated.identity.requestSequence = 1000U;
  CHECK(!applyAppliedImageTransition(cache, unrelated, decision));
  CHECK(applied[0] == 10U);
  CHECK(applied[1] == 11U);
  CHECK(applied[2] == 3U);
  CHECK(aggregate.nextFragmentIndex == 1U);
  CHECK(!aggregate.complete);

  decision = resolveCompletion(
      first,
      CompletionRecord(first.identity, DownstreamOutcome::Applied),
      ready,
      aggregate);
  CHECK(decision.status == CompletionDecisionStatus::OutOfOrder);
  CHECK(!applyAppliedImageTransition(cache, first, decision));

  decision = resolveCompletion(
      final,
      CompletionRecord(final.identity, DownstreamOutcome::Applied),
      ready,
      aggregate);
  CHECK(decision.notice == CompletionNotice::WorkSucceeded);
  CHECK(applyAppliedImageTransition(cache, final, decision));
  CHECK(applied[2] == 12U);
  CHECK(applied[3] == 13U);
  CHECK(aggregate.complete);
  CHECK(!aggregate.failed);
  CHECK(aggregate.noticeEmitted);

  decision = resolveCompletion(
      final,
      CompletionRecord(final.identity, DownstreamOutcome::Applied),
      ready,
      aggregate);
  CHECK(decision.status == CompletionDecisionStatus::AlreadyTerminal);
  CHECK(decision.notice == CompletionNotice::None);
  CHECK(decision.appliedAction == AppliedImageAction::None);

  CompletionAggregate failed(tracked.identity, tracked.delivery);
  decision = resolveCompletion(
      first,
      CompletionRecord(first.identity, DownstreamOutcome::FailedAfterSend),
      ready,
      failed);
  CHECK(decision.current());
  CHECK(decision.appliedAction == AppliedImageAction::None);
  CHECK(decision.desiredDisposition ==
        DesiredImageDisposition::CallerPolicyRequired);
  CHECK(decision.notice == CompletionNotice::WorkFailed);
  CHECK(failed.failed);
  CHECK(!failed.complete);
  CHECK(failed.noticeEmitted);
  CHECK(applyAppliedImageTransition(cache, first, decision));

  decision = resolveCompletion(
      final,
      CompletionRecord(final.identity, DownstreamOutcome::Applied),
      ready,
      failed);
  CHECK(decision.current());
  CHECK(decision.notice == CompletionNotice::None);
  CHECK(decision.appliedAction == AppliedImageAction::MarkAppliedRange);
  CHECK(applyAppliedImageTransition(cache, final, decision));
  CHECK(failed.complete);
  CHECK(failed.failed);
  CHECK(failed.noticeEmitted);

  decision = resolveCompletion(
      final,
      CompletionRecord(final.identity, DownstreamOutcome::Applied),
      ready,
      failed);
  CHECK(decision.status == CompletionDecisionStatus::AlreadyTerminal);
  CHECK(decision.notice == CompletionNotice::None);

  PlannedWriteRequest silent = first;
  silent.delivery = IngressDelivery::SilentOrdered;
  CompletionAggregate silentAggregate(
      silent.identity.work, IngressDelivery::SilentOrdered);
  decision = resolveCompletion(
      silent,
      CompletionRecord(silent.identity, DownstreamOutcome::DefinitelyNotSent),
      ready,
      silentAggregate);
  CHECK(decision.desiredDisposition ==
        DesiredImageDisposition::CallerPolicyRequired);
  CHECK(decision.notice == CompletionNotice::None);

  // A failure from superseded latest-state work must not restore the visible
  // image. The generic layer cannot prove ownership, so rollback is explicit
  // caller policy rather than a default transition.
  PlannedWriteRequest latest = first;
  latest.delivery = IngressDelivery::LatestState;
  latest.finalFragment = true;
  visible[0] = 30U;
  visible[1] = 31U;
  visible[2] = 32U;
  CompletionAggregate latestFailure(
      latest.identity.work, IngressDelivery::LatestState);
  decision = resolveCompletion(
      latest,
      CompletionRecord(latest.identity, DownstreamOutcome::DefinitelyNotSent),
      ready,
      latestFailure);
  CHECK(decision.appliedAction == AppliedImageAction::None);
  CHECK(decision.desiredDisposition ==
        DesiredImageDisposition::CallerPolicyRequired);
  CHECK(applyAppliedImageTransition(cache, latest, decision));
  CHECK(visible[0] == 30U);
  CHECK(visible[1] == 31U);
  CHECK(visible[2] == 32U);

  const DownstreamOutcome otherFailures[] = {
      DownstreamOutcome::SendUncertain,
      DownstreamOutcome::FailedAfterSend};
  for(uint8_t index = 0U; index < 2U; ++index){
    CompletionAggregate outcomeAggregate(
        latest.identity.work, IngressDelivery::LatestState);
    decision = resolveCompletion(
        latest,
        CompletionRecord(latest.identity, otherFailures[index]),
        ready,
        outcomeAggregate);
    CHECK(decision.desiredDisposition ==
          DesiredImageDisposition::CallerPolicyRequired);
    CHECK(decision.notice == CompletionNotice::None);
  }

  // An older snapshot that really applied advances only the applied image;
  // newer visible desired data is left intact for the caller's next work.
  CompletionAggregate latestSuccess(
      latest.identity.work, IngressDelivery::LatestState);
  decision = resolveCompletion(
      latest,
      CompletionRecord(latest.identity, DownstreamOutcome::Applied),
      ready,
      latestSuccess);
  CHECK(decision.appliedAction == AppliedImageAction::MarkAppliedRange);
  CHECK(decision.notice == CompletionNotice::None);
  CHECK(applyAppliedImageTransition(cache, latest, decision));
  CHECK(applied[0] == 10U);
  CHECK(applied[1] == 11U);
  CHECK(visible[0] == 30U);
  CHECK(visible[1] == 31U);

  PlannedWriteRequest wrongTable = latest;
  wrongTable.table = RegisterTable::Coils;
  CHECK(!applyAppliedImageTransition(cache, wrongTable, decision));

  CompletionAggregate wrongAggregate(
      WorkIdentity(32U, 7U), IngressDelivery::Tracked);
  decision = resolveCompletion(
      first,
      CompletionRecord(first.identity, DownstreamOutcome::Applied),
      ready,
      wrongAggregate);
  CHECK(decision.status == CompletionDecisionStatus::AggregateMismatch);

  CompletionAggregate malformedAggregate(
      tracked.identity, static_cast<IngressDelivery>(0xFFU));
  CHECK(!malformedAggregate.valid());
  decision = resolveCompletion(
      first,
      CompletionRecord(first.identity, DownstreamOutcome::Applied),
      ready,
      malformedAggregate);
  CHECK(decision.status == CompletionDecisionStatus::AggregateMismatch);
}

void testCoilCompletionTransition() {
  EndpointRoute route = makeRoute(
      22U, RegisterTable::Coils, 3U, 40U, 1U);
  const RouteTableView table(&route, 1U);
  CHECK(table.validate() == RouteTableStatus::Valid);

  const bool snapshot[] = {true};
  const CoilIngressWorkView work = makeCoilIngressWork(
      WorkIdentity(8U, 4U), 3U, 1U,
      IngressDelivery::Tracked, snapshot);
  const ForwardPlanner planner(table, ForwardPlanOptions(8U, 8U));
  CoilForwardCursor cursor;
  CHECK(planner.begin(work, cursor) == ForwardPlanStatus::Ready);
  PlannedWriteRequest request;
  CHECK(planner.next(cursor, 17U, request) == ForwardNextStatus::Planned);
  CHECK(request.finalFragment);

  bool visible[] = {false, false, false, true};
  bool applied[] = {false, false, false, false};
  const DesiredAppliedCache<bool> cache(
      MutableImageView<bool>(visible, 4U),
      MutableImageView<bool>(applied, 4U));
  CompletionAggregate aggregate(work.identity, work.delivery);
  const CompletionDecision decision = resolveCompletion(
      request,
      CompletionRecord(request.identity, DownstreamOutcome::Applied),
      SessionStateView(true, 4U, 4U, SessionPhase::Ready),
      aggregate);
  CHECK(decision.notice == CompletionNotice::WorkSucceeded);
  CHECK(applyAppliedImageTransition(cache, request, decision));
  CHECK(applied[3]);
  CHECK(visible[3]);
}

enum class TraceKind : uint8_t {
  Planned = 0U,
  Applied,
  Notice,
};

struct TraceEntry {
  TraceKind kind;
  uint32_t sequence;
  uint16_t fragment;
  uint16_t sourceOffset;
  uint8_t endpoint;
  CompletionNotice notice;
};

void testDeterministicTraceSequence() {
  EndpointRoute routes[2];
  routes[0] = makeRoute(
      5U, RegisterTable::HoldingRegisters, 20U, 0U, 2U);
  routes[1] = makeRoute(
      6U, RegisterTable::HoldingRegisters, 22U, 8U, 3U);
  RouteTableView table(routes, 2U);
  CHECK(table.validate() == RouteTableStatus::Valid);
  const uint16_t values[] = {101U, 102U, 103U, 104U, 105U};
  const HoldingIngressWorkView work = makeHoldingIngressWork(
      WorkIdentity(65535U, 12U), 20U, 5U,
      IngressDelivery::LatestState, values);
  ForwardPlanner planner(table, ForwardPlanOptions(2U, 2U));
  HoldingForwardCursor cursor;
  CHECK(planner.begin(work, cursor) == ForwardPlanStatus::Ready);
  CompletionAggregate aggregate(work.identity, work.delivery);
  const SessionStateView ready(true, 12U, 12U, SessionPhase::Ready);

  TraceEntry trace[8] = {};
  uint8_t traceCount = 0U;
  uint32_t sequence = 0xFFFFFFFEUL;
  while(cursor.active){
    sequence = nextNonZeroSequence32(sequence);
    PlannedWriteRequest request;
    CHECK(planner.next(cursor, sequence, request) ==
          ForwardNextStatus::Planned);
    trace[traceCount++] = TraceEntry{
        TraceKind::Planned,
        request.identity.requestSequence,
        request.identity.fragmentIndex,
        request.sourceOffset,
        request.endpointId,
        CompletionNotice::None};
    const CompletionDecision decision = resolveCompletion(
        request,
        CompletionRecord(request.identity, DownstreamOutcome::Applied),
        ready,
        aggregate);
    CHECK(decision.current());
    trace[traceCount++] = TraceEntry{
        TraceKind::Applied,
        request.identity.requestSequence,
        request.identity.fragmentIndex,
        request.sourceOffset,
        request.endpointId,
        decision.notice};
  }

  CHECK(traceCount == 6U);
  CHECK(trace[0].kind == TraceKind::Planned);
  CHECK(trace[0].sequence == 0xFFFFFFFFUL);
  CHECK(trace[0].fragment == 0U);
  CHECK(trace[0].sourceOffset == 0U);
  CHECK(trace[0].endpoint == 5U);
  CHECK(trace[1].kind == TraceKind::Applied);
  CHECK(trace[2].sequence == 1UL);
  CHECK(trace[2].fragment == 1U);
  CHECK(trace[2].sourceOffset == 2U);
  CHECK(trace[2].endpoint == 6U);
  CHECK(trace[4].sequence == 2UL);
  CHECK(trace[4].fragment == 2U);
  CHECK(trace[4].sourceOffset == 4U);
  CHECK(trace[4].endpoint == 6U);
  CHECK(trace[5].notice == CompletionNotice::None);
  CHECK(aggregate.complete);
}

}  // namespace

int main() {
  testIngressDeliveryIdentityAndSession();
  testPlannerPreflightsCompleteRangeAtomically();
  testPlannerChunksAndPreservesSourceOffsets();
  testCompletionDecisionsAndCacheTransitions();
  testCoilCompletionTransition();
  testDeterministicTraceSequence();
  printf("ModbusRTUStoreForwardBridge engine checks: %u\n", g_checks);
  return 0;
}
