#pragma once

// Aggregate, transport-neutral completion facts for planned writes.
//
// This layer accepts only terminal downstream results. It neither retries nor
// rolls the visible image back: both require application policy and, for a
// rollback, proof that newer desired data will not be overwritten. The only
// cache mutation offered here is recording the exact snapshot that a current
// downstream request successfully applied.

#include <stdint.h>

#include <modbus_rtu_bridge/CacheImage.h>
#include <modbus_rtu_bridge/ForwardPlan.h>

namespace ModbusRTUBridge {

// Terminal result facts supplied by the downstream adapter after it has
// finished any application-owned retry policy. Keep transport-specific result
// and exception enums outside the bridge core and map them at the adapter edge.
enum class DownstreamOutcome : uint8_t {
  Applied = 0U,

  // Local validation, queueing, or serialization proves that no downstream
  // device could have observed this request.
  DefinitelyNotSent,

  // The adapter cannot determine whether the request reached the endpoint.
  SendUncertain,

  // A terminal result proves the operation did not apply, but a request was
  // put on the downstream wire (for example, an exception response).
  FailedAfterSend,
};

inline bool isKnownDownstreamOutcome(DownstreamOutcome outcome) {
  switch(outcome){
    case DownstreamOutcome::Applied:
    case DownstreamOutcome::DefinitelyNotSent:
    case DownstreamOutcome::SendUncertain:
    case DownstreamOutcome::FailedAfterSend:
      return true;
    default:
      return false;
  }
}

enum class AppliedImageAction : uint8_t {
  None = 0U,
  MarkAppliedRange,
};

// A failed work may need retry, readback, rollback, or no cache change. The
// generic layer cannot select among them. In particular, it cannot safely
// restore a range without caller-owned per-range generation/ownership proof.
enum class DesiredImageDisposition : uint8_t {
  Unchanged = 0U,
  CallerPolicyRequired,
};

enum class CompletionNotice : uint8_t {
  None = 0U,
  WorkSucceeded,
  WorkFailed,
};

enum class CompletionDecisionStatus : uint8_t {
  Current = 0U,
  IdentityMismatch,
  StaleSession,
  AggregateMismatch,
  OutOfOrder,
  AlreadyTerminal,
  InvalidOutcome,
};

struct CompletionRecord {
  RequestIdentity identity;
  DownstreamOutcome outcome;

  CompletionRecord()
      : identity(), outcome(DownstreamOutcome::SendUncertain) {}

  CompletionRecord(const RequestIdentity& requestIdentity,
                   DownstreamOutcome requestOutcome)
      : identity(requestIdentity), outcome(requestOutcome) {}
};

// Caller-owned, per-work completion state. resolveCompletion() consumes
// fragments strictly in planner order. This is sufficient for the common
// single-runner path without a bitmap or allocation. A concurrent runner must
// reorder completions before resolving them, or own a richer aggregate.
struct CompletionAggregate {
  WorkIdentity work;
  IngressDelivery delivery;
  uint16_t nextFragmentIndex;
  bool failed;
  bool complete;
  bool noticeEmitted;

  CompletionAggregate()
      : work(),
        delivery(IngressDelivery::Rejected),
        nextFragmentIndex(0U),
        failed(false),
        complete(false),
        noticeEmitted(false) {}

  CompletionAggregate(const WorkIdentity& workIdentity,
                      IngressDelivery workDelivery)
      : work(workIdentity),
        delivery(workDelivery),
        nextFragmentIndex(0U),
        failed(false),
        complete(false),
        noticeEmitted(false) {}

  bool valid() const {
    return work.valid() && isAccepted(delivery);
  }
};

struct CompletionDecision {
  CompletionDecisionStatus status;
  RequestIdentity identity;
  AppliedImageAction appliedAction;
  DesiredImageDisposition desiredDisposition;
  CompletionNotice notice;

  CompletionDecision()
      : status(CompletionDecisionStatus::AggregateMismatch),
        identity(),
        appliedAction(AppliedImageAction::None),
        desiredDisposition(DesiredImageDisposition::Unchanged),
        notice(CompletionNotice::None) {}

  CompletionDecision(CompletionDecisionStatus decisionStatus,
                     const RequestIdentity& requestIdentity,
                     AppliedImageAction action,
                     DesiredImageDisposition disposition,
                     CompletionNotice completionNotice)
      : status(decisionStatus),
        identity(requestIdentity),
        appliedAction(action),
        desiredDisposition(disposition),
        notice(completionNotice) {}

  bool current() const {
    return status == CompletionDecisionStatus::Current;
  }
};

namespace CompletionDetail {

inline CompletionDecision rejectedDecision(CompletionDecisionStatus status) {
  return CompletionDecision(
      status,
      RequestIdentity(),
      AppliedImageAction::None,
      DesiredImageDisposition::Unchanged,
      CompletionNotice::None);
}

}  // namespace CompletionDetail

// Resolve one terminal request result into caller-owned aggregate state.
//
// expected must be the immutable planned request retained with the downstream
// queue entry. observed is transport output. aggregate is dedicated to that
// logical work. A success notice is emitted only after every earlier fragment
// has applied and the planner-marked final fragment applies. A failure notice
// is emitted at the first failed fragment. Later fragments that were already
// dispatched may still be resolved to keep the applied image accurate, but
// they cannot turn the failed work into a success or emit a second notice.
inline CompletionDecision resolveCompletion(
    const PlannedWriteRequest& expected,
    const CompletionRecord& observed,
    const SessionStateView& session,
    CompletionAggregate& aggregate) {
  if(!aggregate.valid() || !expected.identity.valid() ||
     !sameWorkIdentity(expected.identity.work, aggregate.work) ||
     expected.delivery != aggregate.delivery){
    return CompletionDetail::rejectedDecision(
        CompletionDecisionStatus::AggregateMismatch);
  }
  if(aggregate.complete){
    return CompletionDetail::rejectedDecision(
        CompletionDecisionStatus::AlreadyTerminal);
  }
  if(!isKnownDownstreamOutcome(observed.outcome)){
    return CompletionDetail::rejectedDecision(
        CompletionDecisionStatus::InvalidOutcome);
  }
  if(!sameRequestIdentity(expected.identity, observed.identity)){
    return CompletionDetail::rejectedDecision(
        CompletionDecisionStatus::IdentityMismatch);
  }
  if(!workCurrent(expected.identity.work, session)){
    return CompletionDetail::rejectedDecision(
        CompletionDecisionStatus::StaleSession);
  }
  if(expected.identity.fragmentIndex != aggregate.nextFragmentIndex){
    return CompletionDetail::rejectedDecision(
        CompletionDecisionStatus::OutOfOrder);
  }

  if(observed.outcome == DownstreamOutcome::Applied){
    if(expected.finalFragment){
      aggregate.complete = true;
      const CompletionNotice notice =
          !aggregate.failed && tracksPublicCompletion(expected.delivery) &&
                  !aggregate.noticeEmitted
              ? CompletionNotice::WorkSucceeded
              : CompletionNotice::None;
      if(notice != CompletionNotice::None){
        aggregate.noticeEmitted = true;
      }
      return CompletionDecision(
          CompletionDecisionStatus::Current,
          expected.identity,
          AppliedImageAction::MarkAppliedRange,
          DesiredImageDisposition::Unchanged,
          notice);
    }

    // A planner cannot produce fragment 65535 from a uint16_t-sized work, but
    // reject corrupted caller-owned aggregate/request state instead of
    // wrapping the expected fragment index back to zero.
    if(aggregate.nextFragmentIndex == 0xFFFFU){
      return CompletionDetail::rejectedDecision(
          CompletionDecisionStatus::AggregateMismatch);
    }
    ++aggregate.nextFragmentIndex;
    return CompletionDecision(
        CompletionDecisionStatus::Current,
        expected.identity,
        AppliedImageAction::MarkAppliedRange,
        DesiredImageDisposition::Unchanged,
        CompletionNotice::None);
  }

  if(!expected.finalFragment &&
     aggregate.nextFragmentIndex == 0xFFFFU){
    return CompletionDetail::rejectedDecision(
        CompletionDecisionStatus::AggregateMismatch);
  }

  const CompletionNotice notice =
      tracksPublicCompletion(expected.delivery) && !aggregate.noticeEmitted
          ? CompletionNotice::WorkFailed
          : CompletionNotice::None;
  aggregate.failed = true;
  aggregate.complete = expected.finalFragment;
  if(notice != CompletionNotice::None){
    aggregate.noticeEmitted = true;
  }
  if(!expected.finalFragment){
    ++aggregate.nextFragmentIndex;
  }
  return CompletionDecision(
      CompletionDecisionStatus::Current,
      expected.identity,
      AppliedImageAction::None,
      DesiredImageDisposition::CallerPolicyRequired,
      notice);
}

// Apply only a successful downstream snapshot to the caller-owned applied
// image. This never changes the visible desired image. The caller must hold
// the same lock it uses for other cache operations.
inline bool applyAppliedImageTransition(
    const DesiredAppliedCache<bool>& cache,
    const PlannedWriteRequest& request,
    const CompletionDecision& decision) {
  if(!decision.current() ||
     !sameRequestIdentity(decision.identity, request.identity)){
    return false;
  }
  if(decision.appliedAction == AppliedImageAction::None){
    return true;
  }
  return request.table == RegisterTable::Coils &&
         cache.markApplied(
             request.upstreamStart, request.coilValues, request.quantity);
}

inline bool applyAppliedImageTransition(
    const DesiredAppliedCache<uint16_t>& cache,
    const PlannedWriteRequest& request,
    const CompletionDecision& decision) {
  if(!decision.current() ||
     !sameRequestIdentity(decision.identity, request.identity)){
    return false;
  }
  if(decision.appliedAction == AppliedImageAction::None){
    return true;
  }
  return request.table == RegisterTable::HoldingRegisters &&
         cache.markApplied(
             request.upstreamStart, request.holdingValues, request.quantity);
}

}  // namespace ModbusRTUBridge
