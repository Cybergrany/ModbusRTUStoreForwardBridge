#pragma once

// Cooperative store-forward orchestration over caller-owned bounded storage.
//
// This facade deliberately has no thread, clock, retry loop, serial transport,
// allocator, or cache rollback policy. nextAction() only previews work.
// admitAction() is the publication point that advances cursors/fairness after a
// caller has execution capacity. complete() accepts one terminal result after
// caller-owned retries and returns a cache/notification decision.

#include <stdint.h>

#include <modbus_rtu_bridge/CompletionLedger.h>
#include <modbus_rtu_bridge/FixedStorage.h>
#include <modbus_rtu_bridge/PollPlanner.h>

namespace ModbusRTUBridge {

inline DownstreamRequest downstreamRequestFor(
    const PlannedWriteRequest& planned,
    void* consistencyContext = nullptr) {
  DownstreamRequest request;
  request.sequence = planned.identity.requestSequence;
  request.operation = planned.operation;
  request.endpointId = planned.endpointId;
  request.startAddress = planned.downstreamStart;
  request.quantity = planned.quantity;
  // Planned payloads are immutable. The transport-neutral executor predates
  // the planner and uses mutable pointers because common Modbus master APIs use
  // one buffer type for reads and writes. A write backend must treat these
  // pointers as read-only; the const_cast exists only at this legacy-shaped
  // adapter edge.
  request.coilBuffer = const_cast<bool*>(planned.coilValues);
  request.registerBuffer = const_cast<uint16_t*>(planned.holdingValues);
  request.coilValue = planned.coilValue;
  request.registerValue = planned.holdingValue;
  request.consistencyContext = consistencyContext;
  return request;
}

inline bool isPollOperation(DownstreamOperation operation) {
  switch(operation){
    case DownstreamOperation::ReadCoils:
    case DownstreamOperation::ReadDiscreteInputs:
    case DownstreamOperation::ReadHoldingRegisters:
    case DownstreamOperation::ReadInputRegisters:
      return true;
    default:
      return false;
  }
}

struct StoreForwardWorkSlot {
  RegisterTable table;
  WorkIdentity workIdentity;
  uint16_t upstreamStart;
  uint16_t quantity;
  IngressDelivery workDelivery;
  const void* snapshot;
  uint16_t nextAddress;
  uint16_t remaining;
  uint16_t sourceOffset;
  uint16_t fragmentIndex;
  bool active;
  void* userContext;

  StoreForwardWorkSlot()
      : table(RegisterTable::Coils),
        workIdentity(),
        upstreamStart(0U),
        quantity(0U),
        workDelivery(IngressDelivery::Rejected),
        snapshot(nullptr),
        nextAddress(0U),
        remaining(0U),
        sourceOffset(0U),
        fragmentIndex(0U),
        active(false),
        userContext(nullptr) {}

  StoreForwardWorkSlot(const CoilIngressWorkView& work,
                       const CoilForwardCursor& cursor,
                       void* context)
      : table(RegisterTable::Coils),
        workIdentity(work.identity),
        upstreamStart(work.start),
        quantity(work.count),
        workDelivery(work.delivery),
        snapshot(work.snapshot.data()),
        nextAddress(cursor.nextAddress),
        remaining(cursor.remaining),
        sourceOffset(cursor.sourceOffset),
        fragmentIndex(cursor.fragmentIndex),
        active(cursor.active),
        userContext(context) {}

  StoreForwardWorkSlot(const HoldingIngressWorkView& work,
                       const HoldingForwardCursor& cursor,
                       void* context)
      : table(RegisterTable::HoldingRegisters),
        workIdentity(work.identity),
        upstreamStart(work.start),
        quantity(work.count),
        workDelivery(work.delivery),
        snapshot(work.snapshot.data()),
        nextAddress(cursor.nextAddress),
        remaining(cursor.remaining),
        sourceOffset(cursor.sourceOffset),
        fragmentIndex(cursor.fragmentIndex),
        active(cursor.active),
        userContext(context) {}

  const WorkIdentity& identity() const { return workIdentity; }

  IngressDelivery delivery() const { return workDelivery; }

  CoilForwardCursor asCoilCursor() const {
    CoilForwardCursor cursor;
    cursor.work = CoilIngressWorkView(
        workIdentity,
        upstreamStart,
        quantity,
        workDelivery,
        ImmutableSnapshotView<bool>(
            static_cast<const bool*>(snapshot), quantity));
    copyStateTo(cursor);
    return cursor;
  }

  HoldingForwardCursor asHoldingCursor() const {
    HoldingForwardCursor cursor;
    cursor.work = HoldingIngressWorkView(
        workIdentity,
        upstreamStart,
        quantity,
        workDelivery,
        ImmutableSnapshotView<uint16_t>(
            static_cast<const uint16_t*>(snapshot), quantity));
    copyStateTo(cursor);
    return cursor;
  }

  void updateFrom(const CoilForwardCursor& cursor) {
    copyStateFrom(cursor);
  }

  void updateFrom(const HoldingForwardCursor& cursor) {
    copyStateFrom(cursor);
  }

 private:
  template<typename Value>
  void copyStateTo(ForwardCursor<Value>& cursor) const {
    cursor.nextAddress = nextAddress;
    cursor.remaining = remaining;
    cursor.sourceOffset = sourceOffset;
    cursor.fragmentIndex = fragmentIndex;
    cursor.active = active;
  }

  template<typename Value>
  void copyStateFrom(const ForwardCursor<Value>& cursor) {
    nextAddress = cursor.nextAddress;
    remaining = cursor.remaining;
    sourceOffset = cursor.sourceOffset;
    fragmentIndex = cursor.fragmentIndex;
    active = cursor.active;
  }
};

struct StoreForwardAction {
  ScheduledActionKind kind;
  uint32_t proposalRevision;
  uint32_t sequence;
  PlannedWriteRequest write;
  DownstreamRequest downstream;
  PollSelection scheduling;
  uint16_t pollCount;
  uint32_t selectedAt;
  bool requiresCompletion;
  void* workContext;

  StoreForwardAction()
      : kind(ScheduledActionKind::None),
        proposalRevision(0UL),
        sequence(0UL),
        write(),
        downstream(),
        scheduling(),
        pollCount(0U),
        selectedAt(0UL),
        requiresCompletion(false),
        workContext(nullptr) {}
};

inline bool sameDownstreamRequest(const DownstreamRequest& lhs,
                                  const DownstreamRequest& rhs) {
  return lhs.sequence == rhs.sequence && lhs.operation == rhs.operation &&
         lhs.endpointId == rhs.endpointId &&
         lhs.startAddress == rhs.startAddress &&
         lhs.quantity == rhs.quantity && lhs.coilBuffer == rhs.coilBuffer &&
         lhs.registerBuffer == rhs.registerBuffer &&
         lhs.coilValue == rhs.coilValue &&
         lhs.registerValue == rhs.registerValue &&
         lhs.consistencyContext == rhs.consistencyContext;
}

inline bool sameStoreForwardAction(const StoreForwardAction& lhs,
                                   const StoreForwardAction& rhs) {
  return lhs.kind == rhs.kind &&
         lhs.proposalRevision == rhs.proposalRevision &&
         lhs.sequence == rhs.sequence &&
         samePlannedWriteRequest(lhs.write, rhs.write) &&
         sameDownstreamRequest(lhs.downstream, rhs.downstream) &&
         lhs.scheduling.kind == rhs.scheduling.kind &&
         lhs.scheduling.pollIndex == rhs.scheduling.pollIndex &&
         lhs.scheduling.originatingPollCount ==
             rhs.scheduling.originatingPollCount &&
         lhs.scheduling.pollCountBound == rhs.scheduling.pollCountBound &&
         lhs.pollCount == rhs.pollCount && lhs.selectedAt == rhs.selectedAt &&
         lhs.requiresCompletion == rhs.requiresCompletion &&
         lhs.workContext == rhs.workContext;
}

enum class StoreForwardAdmitStatus : uint8_t {
  Admitted = 0U,
  InvalidStorage,
  QueueFull,
  PlanRejected,
  LedgerRejected,
};

struct StoreForwardAdmission {
  StoreForwardAdmitStatus status;
  ForwardPlanStatus planStatus;
  LedgerReserveStatus ledgerStatus;

  StoreForwardAdmission()
      : status(StoreForwardAdmitStatus::InvalidStorage),
        planStatus(ForwardPlanStatus::InvalidWork),
        ledgerStatus(LedgerReserveStatus::InvalidWork) {}
};

enum class StoreForwardNextStatus : uint8_t {
  Ready = 0U,
  NoAction,
  ActionInFlight,
  InvalidStorage,
  InvalidPollStorage,
  InvalidPollRequest,
  StaleWork,
  PlanningFailed,
};

enum class StoreForwardActionAdmitStatus : uint8_t {
  Admitted = 0U,
  Consumed,
  ActionInFlight,
  InvalidAction,
  StaleProposal,
  LedgerRejected,
  FairnessRejected,
};

enum class StoreForwardCompleteStatus : uint8_t {
  Completed = 0U,
  NoActionInFlight,
  ActionMismatch,
  InvalidOutcome,
  LedgerRejected,
  StaleSession,
};

struct StoreForwardCompletion {
  StoreForwardCompleteStatus status;
  ScheduledActionKind kind;
  DownstreamOutcome outcome;
  CompletionDecision decision;
  // On StaleSession, this is the ledger snapshot captured before the facade
  // abandons unreachable completion debt. workRetired confirms the slot is no
  // longer live even if this diagnostic snapshot reports outstanding work.
  WorkCompletionSummary work;
  bool workRetired;
  void* workContext;

  StoreForwardCompletion()
      : status(StoreForwardCompleteStatus::NoActionInFlight),
        kind(ScheduledActionKind::None),
        outcome(DownstreamOutcome::SendUncertain),
        decision(),
        work(),
        workRetired(false),
        workContext(nullptr) {}
};

struct StoreForwardRetirement {
  WorkIdentity work;
  RegisterTable table;
  void* userContext;

  StoreForwardRetirement()
      : work(), table(RegisterTable::Coils), userContext(nullptr) {}
};

enum class StoreForwardRetireStatus : uint8_t {
  Retired = 0U,
  Empty,
  ActionInFlight,
  WorkStillCurrent,
};

class StoreForwardBridge {
 public:
  StoreForwardBridge(const ForwardPlanner& forwardPlanner,
                     StoreForwardWorkSlot* workStorage,
                     uint16_t workCapacity,
                     CompletionLedger& completionLedger,
                     const PollPlanner& pollPlanner = PollPlanner())
      : forwardPlanner_(forwardPlanner),
        workQueue_(workStorage, workCapacity),
        completionLedger_(&completionLedger),
        pollPlanner_(pollPlanner),
        pollState_(),
        requestSequence_(0UL),
        revision_(1UL),
        inFlight_(false),
        inFlightAction_(),
        inFlightWork_() {}

  // Copies would alias work/ledger storage while sequences and in-flight state
  // diverge. Keep one facade instance per scheduling domain.
  StoreForwardBridge(const StoreForwardBridge&) = delete;
  StoreForwardBridge& operator=(const StoreForwardBridge&) = delete;

  bool validStorage() const {
    return workQueue_.validStorage() && completionLedger_ != nullptr &&
           completionLedger_->validStorage();
  }

  uint16_t queuedWork() const { return workQueue_.size(); }
  bool actionInFlight() const { return inFlight_; }
  uint32_t lastRequestSequence() const { return requestSequence_; }
  const PollPlannerState& pollState() const { return pollState_; }

  StoreForwardAdmission admitWork(const CoilIngressWorkView& work,
                                  const SessionStateView& session,
                                  void* userContext = nullptr) {
    StoreForwardAdmission result;
    if(!validStorage()){
      return result;
    }
    if(workQueue_.full()){
      result.status = StoreForwardAdmitStatus::QueueFull;
      return result;
    }

    CoilForwardCursor cursor;
    result.planStatus = forwardPlanner_.begin(work, session, cursor);
    if(result.planStatus != ForwardPlanStatus::Ready){
      result.status = StoreForwardAdmitStatus::PlanRejected;
      return result;
    }

    result.ledgerStatus = completionLedger_->reserve(
        work.identity, work.delivery);
    if(result.ledgerStatus != LedgerReserveStatus::Reserved){
      result.status = StoreForwardAdmitStatus::LedgerRejected;
      return result;
    }

    const StoreForwardWorkSlot slot(work, cursor, userContext);
    if(!workQueue_.tryPush(slot)){
      // A cooperative caller cannot race the pre-check, but release the ledger
      // slot defensively if corrupt storage violates that assumption.
      completionLedger_->abandon(work.identity);
      result.status = StoreForwardAdmitStatus::QueueFull;
      return result;
    }
    advanceRevision();
    result.status = StoreForwardAdmitStatus::Admitted;
    return result;
  }

  StoreForwardAdmission admitWork(const HoldingIngressWorkView& work,
                                  const SessionStateView& session,
                                  void* userContext = nullptr) {
    StoreForwardAdmission result;
    if(!validStorage()){
      return result;
    }
    if(workQueue_.full()){
      result.status = StoreForwardAdmitStatus::QueueFull;
      return result;
    }

    HoldingForwardCursor cursor;
    result.planStatus = forwardPlanner_.begin(work, session, cursor);
    if(result.planStatus != ForwardPlanStatus::Ready){
      result.status = StoreForwardAdmitStatus::PlanRejected;
      return result;
    }

    result.ledgerStatus = completionLedger_->reserve(
        work.identity, work.delivery);
    if(result.ledgerStatus != LedgerReserveStatus::Reserved){
      result.status = StoreForwardAdmitStatus::LedgerRejected;
      return result;
    }

    const StoreForwardWorkSlot slot(work, cursor, userContext);
    if(!workQueue_.tryPush(slot)){
      completionLedger_->abandon(work.identity);
      result.status = StoreForwardAdmitStatus::QueueFull;
      return result;
    }
    advanceRevision();
    result.status = StoreForwardAdmitStatus::Admitted;
    return result;
  }

  // Preview the next action without consuming work, advancing round-robin
  // state, or creating completion debt. The caller may reject this proposal
  // (for example because its transport queue filled) and call nextAction again.
  StoreForwardNextStatus nextAction(
      const SessionStateView& session,
      uint32_t now,
      const PollCandidate* polls,
      uint16_t pollCount,
      StoreForwardAction& action,
      void* writeConsistencyContext = nullptr) const {
    CoilForwardCursor coilCursor;
    HoldingForwardCursor holdingCursor;
    return previewAction(session,
                         now,
                         polls,
                         pollCount,
                         action,
                         coilCursor,
                         holdingCursor,
                         writeConsistencyContext);
  }

  // Commit exactly one previously previewed action after the caller has
  // reserved downstream execution capacity. This is the only method that
  // advances a forward cursor, request sequence, fairness cursor, or ledger
  // outstanding count.
  StoreForwardActionAdmitStatus admitAction(
      const StoreForwardAction& action,
      const SessionStateView& session,
      const PollCandidate* polls,
      uint16_t pollCount) {
    if(inFlight_){
      return StoreForwardActionAdmitStatus::ActionInFlight;
    }
    if(action.kind == ScheduledActionKind::None){
      return StoreForwardActionAdmitStatus::InvalidAction;
    }
    if(action.proposalRevision != revision_){
      return StoreForwardActionAdmitStatus::StaleProposal;
    }
    StoreForwardAction currentProposal;
    CoilForwardCursor committedCoilCursor;
    HoldingForwardCursor committedHoldingCursor;
    if(previewAction(session,
                     action.selectedAt,
                     polls,
                     pollCount,
                     currentProposal,
                     committedCoilCursor,
                     committedHoldingCursor,
                     action.downstream.consistencyContext) !=
           StoreForwardNextStatus::Ready ||
       !sameStoreForwardAction(action, currentProposal) ||
       action.kind != action.scheduling.kind){
      return StoreForwardActionAdmitStatus::StaleProposal;
    }

    if(action.kind == ScheduledActionKind::Poll &&
       !action.requiresCompletion){
      if(action.sequence != 0UL ||
         !pollPlanner_.commit(action.scheduling,
                              action.pollCount,
                              action.selectedAt,
                              pollState_)){
        return StoreForwardActionAdmitStatus::FairnessRejected;
      }
      advanceRevision();
      return StoreForwardActionAdmitStatus::Consumed;
    }
    if(action.sequence == 0UL ||
       action.sequence != nextNonZeroSequence32(requestSequence_)){
      return StoreForwardActionAdmitStatus::StaleProposal;
    }

    if(action.kind == ScheduledActionKind::ForwardWrite){
      // Exact re-preview above proves this is a ForwardWrite fairness commit;
      // therefore commit cannot reject after recordIssued creates debt.
      if(action.scheduling.kind != ScheduledActionKind::ForwardWrite){
        return StoreForwardActionAdmitStatus::InvalidAction;
      }
      StoreForwardWorkSlot* front = workQueue_.front();
      if(front == nullptr){
        return StoreForwardActionAdmitStatus::StaleProposal;
      }
      const PlannedWriteRequest& committedRequest = currentProposal.write;
      const PollPlannerState previousPollState = pollState_;
      if(!pollPlanner_.commit(action.scheduling,
                              action.pollCount,
                              action.selectedAt,
                              pollState_)){
        return StoreForwardActionAdmitStatus::FairnessRejected;
      }
      if(completionLedger_->recordIssued(committedRequest) !=
         LedgerIssueStatus::Recorded){
        // Keep admission atomic if externally shared/corrupted ledger state
        // changed after preview. No work cursor or request debt is committed.
        pollState_ = previousPollState;
        return StoreForwardActionAdmitStatus::LedgerRejected;
      }
      if(front->table == RegisterTable::Coils){
        front->updateFrom(committedCoilCursor);
      }else{
        front->updateFrom(committedHoldingCursor);
      }
      inFlightWork_ = committedRequest.identity.work;
    }else if(action.kind == ScheduledActionKind::Poll){
      if(action.scheduling.kind != ScheduledActionKind::Poll ||
         action.downstream.sequence != action.sequence){
        return StoreForwardActionAdmitStatus::InvalidAction;
      }
      if(!isPollOperation(action.downstream.operation) ||
         validateDownstreamRequest(action.downstream) !=
             DownstreamRequestStatus::Valid){
        return StoreForwardActionAdmitStatus::InvalidAction;
      }
      if(!action.requiresCompletion ||
         !pollPlanner_.commit(action.scheduling,
                              action.pollCount,
                              action.selectedAt,
                              pollState_)){
        return StoreForwardActionAdmitStatus::FairnessRejected;
      }
      inFlightWork_ = WorkIdentity();
    }else{
      return StoreForwardActionAdmitStatus::InvalidAction;
    }

    requestSequence_ = action.sequence;
    inFlight_ = true;
    inFlightAction_ = action;
    advanceRevision();
    return StoreForwardActionAdmitStatus::Admitted;
  }

  // outcome must be terminal after the caller's retry policy. This method does
  // not retry, update desired cache state, or execute a backend.
  StoreForwardCompletion complete(const StoreForwardAction& admittedAction,
                                  DownstreamOutcome outcome,
                                  const SessionStateView& session) {
    StoreForwardCompletion result;
    result.kind = inFlight_ ? inFlightAction_.kind : admittedAction.kind;
    result.outcome = outcome;
    result.workContext = inFlight_ ? inFlightAction_.workContext : nullptr;
    if(!inFlight_){
      return result;
    }
    if(!sameStoreForwardAction(admittedAction, inFlightAction_)){
      result.status = StoreForwardCompleteStatus::ActionMismatch;
      return result;
    }
    if(!isKnownDownstreamOutcome(outcome)){
      result.status = StoreForwardCompleteStatus::InvalidOutcome;
      return result;
    }

    if(inFlightAction_.kind == ScheduledActionKind::Poll){
      clearInFlight();
      advanceRevision();
      result.status = StoreForwardCompleteStatus::Completed;
      return result;
    }
    if(inFlightAction_.kind != ScheduledActionKind::ForwardWrite ||
       !sameWorkIdentity(admittedAction.write.identity.work, inFlightWork_)){
      result.status = StoreForwardCompleteStatus::ActionMismatch;
      return result;
    }

    const LedgerResolution resolution = completionLedger_->resolve(
        admittedAction.write,
        CompletionRecord(admittedAction.write.identity, outcome),
        session);
    result.decision = resolution.decision;
    result.work = resolution.summary;
    if(resolution.status != LedgerResolveStatus::Resolved){
      if(resolution.status == LedgerResolveStatus::CompletionRejected &&
         resolution.decision.status ==
             CompletionDecisionStatus::StaleSession){
        // An old generation can never become current again. Drop its queue and
        // ledger state after rejecting every cache/notice transition.
        completionLedger_->abandon(inFlightWork_);
        popFrontIf(inFlightWork_);
        clearInFlight();
        advanceRevision();
        result.status = StoreForwardCompleteStatus::StaleSession;
        result.workRetired = true;
        return result;
      }
      result.status = StoreForwardCompleteStatus::LedgerRejected;
      return result;
    }

    clearInFlight();
    if(outcome != DownstreamOutcome::Applied){
      // Terminal failure settles the logical work and prevents unissued
      // fragments from appearing after it. This cooperative facade permits
      // one exact outstanding fragment per work.
      completionLedger_->closePlanningAfterFailure(
          admittedAction.write.identity.work);
      completionLedger_->summary(admittedAction.write.identity.work,
                                 result.work);
    }
    if(result.work.drained){
      completionLedger_->release(admittedAction.write.identity.work);
      popFrontIf(admittedAction.write.identity.work);
      result.workRetired = true;
    }
    advanceRevision();
    result.status = StoreForwardCompleteStatus::Completed;
    return result;
  }

  // Remove one idle old-session work and return its identity/context so the
  // caller can reclaim snapshot storage. In-flight stale work is handled by
  // complete(), which first rejects every cache/notice transition.
  StoreForwardRetireStatus discardOneStaleWork(
      const SessionStateView& session,
      StoreForwardRetirement& retired) {
    retired = StoreForwardRetirement();
    if(inFlight_){
      return StoreForwardRetireStatus::ActionInFlight;
    }
    StoreForwardWorkSlot* front = workQueue_.front();
    if(front == nullptr){
      return StoreForwardRetireStatus::Empty;
    }
    if(workCurrent(front->identity(), session)){
      return StoreForwardRetireStatus::WorkStillCurrent;
    }
    retireFront(retired);
    return StoreForwardRetireStatus::Retired;
  }

  // Explicitly abandon one idle front work regardless of session. Prefer this
  // one-at-a-time API over reset() when each snapshot/context has an individual
  // lifetime that must be reclaimed.
  StoreForwardRetireStatus abandonFront(StoreForwardRetirement& retired) {
    retired = StoreForwardRetirement();
    if(inFlight_){
      return StoreForwardRetireStatus::ActionInFlight;
    }
    if(workQueue_.front() == nullptr){
      return StoreForwardRetireStatus::Empty;
    }
    retireFront(retired);
    return StoreForwardRetireStatus::Retired;
  }

  // Reset only while no action is executing. Every queued work identity owned
  // by this facade is explicitly abandoned in the shared ledger. This bulk
  // convenience returns no per-work context, so use it only when snapshot
  // storage can be reclaimed as one pool; otherwise call abandonFront().
  bool reset() {
    if(inFlight_){
      return false;
    }
    while(workQueue_.front() != nullptr){
      completionLedger_->abandon(workQueue_.front()->identity());
      workQueue_.popFront();
    }
    pollState_ = PollPlannerState();
    advanceRevision();
    return true;
  }

 private:
  StoreForwardNextStatus previewAction(
      const SessionStateView& session,
      uint32_t now,
      const PollCandidate* polls,
      uint16_t pollCount,
      StoreForwardAction& action,
      CoilForwardCursor& nextCoilCursor,
      HoldingForwardCursor& nextHoldingCursor,
      void* writeConsistencyContext) const {
    action = StoreForwardAction();
    nextCoilCursor = CoilForwardCursor();
    nextHoldingCursor = HoldingForwardCursor();
    if(!validStorage()){
      return StoreForwardNextStatus::InvalidStorage;
    }
    if(inFlight_){
      return StoreForwardNextStatus::ActionInFlight;
    }

    const StoreForwardWorkSlot* front = workQueue_.front();
    if(front != nullptr && !workCurrent(front->identity(), session)){
      return StoreForwardNextStatus::StaleWork;
    }

    PollSelection selection;
    const PollSelectStatus selected = pollPlanner_.select(
        now, front != nullptr, polls, pollCount, pollState_, selection);
    if(selected == PollSelectStatus::InvalidStorage){
      return StoreForwardNextStatus::InvalidPollStorage;
    }
    if(selected == PollSelectStatus::NoAction){
      return StoreForwardNextStatus::NoAction;
    }

    const uint32_t proposedSequence =
        nextNonZeroSequence32(requestSequence_);
    action.kind = selection.kind;
    action.proposalRevision = revision_;
    action.scheduling = selection;
    action.pollCount = pollCount;
    action.selectedAt = now;

    if(selection.kind == ScheduledActionKind::Poll){
      if(polls == nullptr || selection.pollIndex >= pollCount){
        action = StoreForwardAction();
        return StoreForwardNextStatus::InvalidPollStorage;
      }
      action.requiresCompletion = polls[selection.pollIndex].requiresDispatch;
      if(!action.requiresCompletion){
        // The application deliberately consumes this poll phase without a
        // wire request. admitAction() will commit fairness/lastPoll state and
        // return Consumed without reserving an in-flight action.
        return StoreForwardNextStatus::Ready;
      }
      action.sequence = proposedSequence;
      action.downstream = polls[selection.pollIndex].request;
      action.downstream.sequence = action.sequence;
      if(!isPollOperation(action.downstream.operation) ||
         validateDownstreamRequest(action.downstream) !=
             DownstreamRequestStatus::Valid){
        action = StoreForwardAction();
        return StoreForwardNextStatus::InvalidPollRequest;
      }
      return StoreForwardNextStatus::Ready;
    }

    if(selection.kind != ScheduledActionKind::ForwardWrite || front == nullptr){
      action = StoreForwardAction();
      return StoreForwardNextStatus::PlanningFailed;
    }
    action.sequence = proposedSequence;
    action.requiresCompletion = true;
    action.workContext = front->userContext;
    ForwardNextStatus planned = ForwardNextStatus::TopologyChanged;
    if(front->table == RegisterTable::Coils){
      nextCoilCursor = front->asCoilCursor();
      planned = forwardPlanner_.next(
          nextCoilCursor, proposedSequence, action.write);
    }else if(front->table == RegisterTable::HoldingRegisters){
      nextHoldingCursor = front->asHoldingCursor();
      planned = forwardPlanner_.next(
          nextHoldingCursor, proposedSequence, action.write);
    }
    if(planned != ForwardNextStatus::Planned){
      action = StoreForwardAction();
      return StoreForwardNextStatus::PlanningFailed;
    }
    action.downstream = downstreamRequestFor(
        action.write, writeConsistencyContext);
    return StoreForwardNextStatus::Ready;
  }

  void advanceRevision() {
    revision_ = nextNonZeroSequence32(revision_);
  }

  void clearInFlight() {
    inFlight_ = false;
    inFlightAction_ = StoreForwardAction();
    inFlightWork_ = WorkIdentity();
  }

  void popFrontIf(const WorkIdentity& work) {
    StoreForwardWorkSlot* front = workQueue_.front();
    if(front != nullptr && sameWorkIdentity(front->identity(), work)){
      workQueue_.popFront();
    }
  }

  void retireFront(StoreForwardRetirement& retired) {
    StoreForwardWorkSlot* front = workQueue_.front();
    if(front == nullptr){
      return;
    }
    retired.work = front->identity();
    retired.table = front->table;
    retired.userContext = front->userContext;
    completionLedger_->abandon(front->identity());
    workQueue_.popFront();
    advanceRevision();
  }

  ForwardPlanner forwardPlanner_;
  FixedRingQueue<StoreForwardWorkSlot> workQueue_;
  CompletionLedger* completionLedger_;
  PollPlanner pollPlanner_;
  PollPlannerState pollState_;
  uint32_t requestSequence_;
  uint32_t revision_;
  bool inFlight_;
  StoreForwardAction inFlightAction_;
  WorkIdentity inFlightWork_;
};

}  // namespace ModbusRTUBridge
