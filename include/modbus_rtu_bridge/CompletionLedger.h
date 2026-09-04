#pragma once

// Bounded completion tracking for several concurrently admitted work items.
//
// The ledger owns no storage and performs no retry or rollback. A runner first
// reserves a slot, records each request only after downstream queue admission,
// and resolves each request only after its retry policy reaches a terminal
// transport result. Failure can settle a public result before planning is
// closed; the slot remains occupied until its exact issued request completes
// and the caller closes planning, or explicitly abandons a dead session.

#include <stdint.h>

#include <modbus_rtu_bridge/CompletionTransition.h>

namespace ModbusRTUBridge {

// One logical work result. Names deliberately describe what the runner can
// prove, not what it hopes happened on the wire.
enum class WorkCompletionOutcome : uint8_t {
  Pending = 0U,
  Applied,
  DefinitelyNotSent,
  UncertainSend,
  TerminalFailure,
};

inline WorkCompletionOutcome workOutcomeFor(DownstreamOutcome outcome) {
  switch(outcome){
    case DownstreamOutcome::Applied:
      return WorkCompletionOutcome::Applied;
    case DownstreamOutcome::DefinitelyNotSent:
      return WorkCompletionOutcome::DefinitelyNotSent;
    case DownstreamOutcome::SendUncertain:
      return WorkCompletionOutcome::UncertainSend;
    case DownstreamOutcome::FailedAfterSend:
    default:
      return WorkCompletionOutcome::TerminalFailure;
  }
}

// When different fragments end differently, preserve the strongest loss of
// certainty. A definite post-send failure dominates an uncertain send, which
// dominates proof that nothing was sent. Applied is successful only when no
// failure outcome was observed for the work.
inline WorkCompletionOutcome combineWorkOutcomes(
    WorkCompletionOutcome current,
    DownstreamOutcome observed) {
  const WorkCompletionOutcome next = workOutcomeFor(observed);
  if(current == WorkCompletionOutcome::Pending ||
     current == WorkCompletionOutcome::Applied){
    return next;
  }
  if(next == WorkCompletionOutcome::TerminalFailure ||
     current == WorkCompletionOutcome::TerminalFailure){
    return WorkCompletionOutcome::TerminalFailure;
  }
  if(next == WorkCompletionOutcome::UncertainSend ||
     current == WorkCompletionOutcome::UncertainSend){
    return WorkCompletionOutcome::UncertainSend;
  }
  return WorkCompletionOutcome::DefinitelyNotSent;
}

struct WorkCompletionSummary {
  WorkIdentity work;
  WorkCompletionOutcome outcome;
  uint16_t outstandingRequests;
  bool settled;
  bool drained;

  WorkCompletionSummary()
      : work(),
        outcome(WorkCompletionOutcome::Pending),
        outstandingRequests(0U),
        settled(false),
        drained(false) {}
};

struct CompletionLedgerSlot {
  CompletionAggregate aggregate;
  PlannedWriteRequest issuedRequest;
  WorkCompletionOutcome observedOutcome;
  uint16_t nextIssuedFragment;
  uint16_t outstandingRequests;
  bool occupied;
  bool planningClosed;

  CompletionLedgerSlot()
      : aggregate(),
        issuedRequest(),
        observedOutcome(WorkCompletionOutcome::Pending),
        nextIssuedFragment(0U),
        outstandingRequests(0U),
        occupied(false),
        planningClosed(false) {}
};

enum class LedgerReserveStatus : uint8_t {
  Reserved = 0U,
  InvalidStorage,
  InvalidWork,
  DuplicateWork,
  Full,
};

enum class LedgerIssueStatus : uint8_t {
  Recorded = 0U,
  WorkNotFound,
  InvalidRequest,
  OutOfOrder,
  PlanningClosed,
  OutstandingRequestExists,
  FragmentIndexOverflow,
};

enum class LedgerResolveStatus : uint8_t {
  Resolved = 0U,
  WorkNotFound,
  InvalidOutcome,
  NoOutstandingRequest,
  RequestMismatch,
  CompletionRejected,
};

enum class LedgerReleaseStatus : uint8_t {
  Released = 0U,
  WorkNotFound,
  StillActive,
};

struct LedgerResolution {
  LedgerResolveStatus status;
  CompletionDecision decision;
  WorkCompletionSummary summary;

  LedgerResolution()
      : status(LedgerResolveStatus::WorkNotFound),
        decision(),
        summary() {}
};

class CompletionLedger {
 public:
  CompletionLedger() : slots_(nullptr), capacity_(0U), size_(0U) {}

  CompletionLedger(CompletionLedgerSlot* slots, uint16_t capacity)
      : slots_(slots), capacity_(capacity), size_(0U) {
    clear();
  }

  // Copies would alias caller slots with independent occupancy counters.
  CompletionLedger(const CompletionLedger&) = delete;
  CompletionLedger& operator=(const CompletionLedger&) = delete;

  bool validStorage() const {
    return capacity_ == 0U || slots_ != nullptr;
  }

  uint16_t capacity() const { return capacity_; }
  uint16_t size() const { return size_; }
  bool empty() const { return size_ == 0U; }
  bool full() const { return size_ == capacity_; }

  LedgerReserveStatus reserve(const WorkIdentity& work,
                              IngressDelivery delivery) {
    if(!validStorage()){
      return LedgerReserveStatus::InvalidStorage;
    }
    if(!work.valid() || !isAccepted(delivery)){
      return LedgerReserveStatus::InvalidWork;
    }
    if(find(work) != nullptr){
      return LedgerReserveStatus::DuplicateWork;
    }
    CompletionLedgerSlot* freeSlot = firstFree();
    if(freeSlot == nullptr){
      return LedgerReserveStatus::Full;
    }
    *freeSlot = CompletionLedgerSlot();
    freeSlot->aggregate = CompletionAggregate(work, delivery);
    freeSlot->occupied = true;
    ++size_;
    return LedgerReserveStatus::Reserved;
  }

  // Record only requests that were actually admitted to the downstream
  // execution path. Merely previewing/planning a request must not call this.
  LedgerIssueStatus recordIssued(const PlannedWriteRequest& request) {
    if(!request.identity.valid() || !isAccepted(request.delivery)){
      return LedgerIssueStatus::InvalidRequest;
    }
    CompletionLedgerSlot* slot = find(request.identity.work);
    if(slot == nullptr){
      return LedgerIssueStatus::WorkNotFound;
    }
    if(slot->planningClosed){
      return LedgerIssueStatus::PlanningClosed;
    }
    // The aggregate resolves fragments in strict order. Keeping one exact
    // issued request in each work slot makes that contract self-validating
    // without allocating a per-fragment bitmap/queue. Different work slots may
    // still have one request outstanding concurrently.
    if(slot->outstandingRequests != 0U){
      return LedgerIssueStatus::OutstandingRequestExists;
    }
    if(request.delivery != slot->aggregate.delivery ||
       request.identity.fragmentIndex != slot->nextIssuedFragment){
      return LedgerIssueStatus::OutOfOrder;
    }
    if(slot->nextIssuedFragment == 0xFFFFU && !request.finalFragment){
      return LedgerIssueStatus::FragmentIndexOverflow;
    }
    slot->issuedRequest = request;
    slot->outstandingRequests = 1U;
    if(request.finalFragment){
      slot->planningClosed = true;
    }else{
      ++slot->nextIssuedFragment;
    }
    return LedgerIssueStatus::Recorded;
  }

  // Stop admitting new fragments only after a resolved non-Applied terminal
  // outcome. Pre-issue cancellation has no outcome and must use abandon(); an
  // Applied partial work must continue to its planner-marked final fragment.
  bool closePlanningAfterFailure(const WorkIdentity& work) {
    CompletionLedgerSlot* slot = find(work);
    if(slot == nullptr ||
       slot->observedOutcome == WorkCompletionOutcome::Pending ||
       slot->observedOutcome == WorkCompletionOutcome::Applied){
      return false;
    }
    slot->planningClosed = true;
    return true;
  }

  LedgerResolution resolve(const PlannedWriteRequest& expected,
                           const CompletionRecord& observed,
                           const SessionStateView& session) {
    LedgerResolution result;
    CompletionLedgerSlot* slot = find(expected.identity.work);
    if(slot == nullptr){
      result.status = LedgerResolveStatus::WorkNotFound;
      return result;
    }
    if(!isKnownDownstreamOutcome(observed.outcome)){
      result.status = LedgerResolveStatus::InvalidOutcome;
      result.summary = summarize(*slot);
      return result;
    }
    if(slot->outstandingRequests == 0U){
      result.status = LedgerResolveStatus::NoOutstandingRequest;
      result.summary = summarize(*slot);
      return result;
    }
    if(!samePlannedWriteRequest(slot->issuedRequest, expected)){
      result.status = LedgerResolveStatus::RequestMismatch;
      result.summary = summarize(*slot);
      return result;
    }

    result.decision = resolveCompletion(
        slot->issuedRequest, observed, session, slot->aggregate);
    if(!result.decision.current()){
      result.status = LedgerResolveStatus::CompletionRejected;
      result.summary = summarize(*slot);
      return result;
    }

    slot->observedOutcome = combineWorkOutcomes(
        slot->observedOutcome, observed.outcome);
    slot->outstandingRequests = 0U;
    slot->issuedRequest = PlannedWriteRequest();
    result.status = LedgerResolveStatus::Resolved;
    result.summary = summarize(*slot);
    return result;
  }

  bool summary(const WorkIdentity& work, WorkCompletionSummary& result) const {
    const CompletionLedgerSlot* slot = find(work);
    if(slot == nullptr){
      result = WorkCompletionSummary();
      return false;
    }
    result = summarize(*slot);
    return true;
  }

  LedgerReleaseStatus release(const WorkIdentity& work) {
    CompletionLedgerSlot* slot = find(work);
    if(slot == nullptr){
      return LedgerReleaseStatus::WorkNotFound;
    }
    if(!summarize(*slot).drained){
      return LedgerReleaseStatus::StillActive;
    }
    releaseSlot(*slot);
    return LedgerReleaseStatus::Released;
  }

  // Lifecycle teardown may make old-session completions permanently invalid.
  // abandon() is deliberately explicit because it discards outstanding debt.
  bool abandon(const WorkIdentity& work) {
    CompletionLedgerSlot* slot = find(work);
    if(slot == nullptr){
      return false;
    }
    releaseSlot(*slot);
    return true;
  }

  // Bulk teardown helpers make stale-session capacity cleanup explicit. They
  // intentionally abandon outstanding debt and therefore belong only on a
  // lifecycle boundary where those completions can never become current.
  uint16_t abandonGeneration(uint16_t generation) {
    uint16_t removed = 0U;
    if(slots_ == nullptr || generation == 0U){
      return removed;
    }
    for(uint16_t index = 0U; index < capacity_; ++index){
      if(slots_[index].occupied &&
         slots_[index].aggregate.work.sessionGeneration == generation){
        releaseSlot(slots_[index]);
        ++removed;
      }
    }
    return removed;
  }

  uint16_t abandonNotCurrent(const SessionStateView& session) {
    uint16_t removed = 0U;
    if(slots_ == nullptr){
      return removed;
    }
    for(uint16_t index = 0U; index < capacity_; ++index){
      if(slots_[index].occupied &&
         !workCurrent(slots_[index].aggregate.work, session)){
        releaseSlot(slots_[index]);
        ++removed;
      }
    }
    return removed;
  }

  void clear() {
    size_ = 0U;
    if(slots_ == nullptr){
      return;
    }
    for(uint16_t index = 0U; index < capacity_; ++index){
      slots_[index] = CompletionLedgerSlot();
    }
  }

 private:
  CompletionLedgerSlot* firstFree() {
    if(slots_ == nullptr){
      return nullptr;
    }
    for(uint16_t index = 0U; index < capacity_; ++index){
      if(!slots_[index].occupied){
        return slots_ + index;
      }
    }
    return nullptr;
  }

  CompletionLedgerSlot* find(const WorkIdentity& work) {
    if(slots_ == nullptr || !work.valid()){
      return nullptr;
    }
    for(uint16_t index = 0U; index < capacity_; ++index){
      if(slots_[index].occupied &&
         sameWorkIdentity(slots_[index].aggregate.work, work)){
        return slots_ + index;
      }
    }
    return nullptr;
  }

  const CompletionLedgerSlot* find(const WorkIdentity& work) const {
    if(slots_ == nullptr || !work.valid()){
      return nullptr;
    }
    for(uint16_t index = 0U; index < capacity_; ++index){
      if(slots_[index].occupied &&
         sameWorkIdentity(slots_[index].aggregate.work, work)){
        return slots_ + index;
      }
    }
    return nullptr;
  }

  static WorkCompletionSummary summarize(const CompletionLedgerSlot& slot) {
    WorkCompletionSummary result;
    result.work = slot.aggregate.work;
    result.outstandingRequests = slot.outstandingRequests;
    result.drained = slot.planningClosed && slot.outstandingRequests == 0U;
    const bool failed = slot.observedOutcome != WorkCompletionOutcome::Pending &&
                        slot.observedOutcome != WorkCompletionOutcome::Applied;
    result.settled = failed || result.drained;
    result.outcome = result.settled
        ? slot.observedOutcome
        : WorkCompletionOutcome::Pending;
    return result;
  }

  void releaseSlot(CompletionLedgerSlot& slot) {
    slot = CompletionLedgerSlot();
    --size_;
  }

  CompletionLedgerSlot* slots_;
  uint16_t capacity_;
  uint16_t size_;
};

}  // namespace ModbusRTUBridge
