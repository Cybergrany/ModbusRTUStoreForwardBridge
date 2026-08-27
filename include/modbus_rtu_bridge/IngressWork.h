#pragma once

// Immutable ingress-work and session identity contracts.
//
// This header deliberately contains no queue, lock, clock, retry, or transport
// behavior. A caller may place these small views in its own cooperative or
// threaded pipeline, but the pointed-to snapshot must remain immutable until
// every planned request that borrows it has completed.

#include <stdint.h>

namespace ModbusRTUBridge {

// How an accepted upstream write is delivered by a cached forwarder.
//
// Delivery is independent of the downstream wire function. In particular,
// LatestState permits coalescing but does not require a non-standard Modbus
// function code on the downstream network.
enum class IngressDelivery : uint8_t {
  // Ordered work with a caller-visible success/failure completion.
  Tracked = 0U,

  // Ordered work whose downstream result still controls cache state, but for
  // which no caller-visible completion exists (for example, a broadcast).
  SilentOrdered = 1U,

  // Latest-state work that may be coalesced. A successful terminal result may
  // still advance the applied image, but no public completion is owed.
  LatestState = 2U,

  // Work rejected before admission/cache mutation. It is diagnostic-only and
  // must never be forwarded or converted into completion debt.
  Rejected = 3U,
};

// suppressPublicCompletion is useful for an application-owned control write:
// it still has ordered result semantics, but it must not create endpoint debt.
inline IngressDelivery classifyAcceptedIngress(bool latestState,
                                                bool responseRequired,
                                                bool suppressPublicCompletion) {
  if(latestState){
    return IngressDelivery::LatestState;
  }
  if(!responseRequired || suppressPublicCompletion){
    return IngressDelivery::SilentOrdered;
  }
  return IngressDelivery::Tracked;
}

inline bool isAccepted(IngressDelivery delivery) {
  return delivery != IngressDelivery::Rejected;
}

inline bool isLatestState(IngressDelivery delivery) {
  return delivery == IngressDelivery::LatestState;
}

inline bool tracksPublicCompletion(IngressDelivery delivery) {
  return delivery == IngressDelivery::Tracked;
}

inline bool requiresOrderedDelivery(IngressDelivery delivery) {
  return delivery == IngressDelivery::Tracked ||
         delivery == IngressDelivery::SilentOrdered;
}

inline bool preservesSourceOrder(IngressDelivery delivery) {
  return requiresOrderedDelivery(delivery);
}

// Advance a wrapping 16-bit generation/token while reserving zero as invalid.
inline uint16_t nextNonZeroSerial16(uint16_t value) {
  const uint16_t next = static_cast<uint16_t>(value + 1U);
  return next == 0U ? 1U : next;
}

// RFC-1982-style ordering for non-zero 16-bit serials. Live values compared by
// a caller must remain less than half the serial space apart. Zero sorts after
// every valid value and is never considered older than another value.
inline bool nonZeroSerial16Before(uint16_t lhs, uint16_t rhs) {
  if(lhs == rhs || lhs == 0U){
    return false;
  }
  if(rhs == 0U){
    return true;
  }
  // Unsigned subtraction gives the forward distance modulo 2^16 without an
  // implementation-defined conversion to a signed 16-bit value. Exactly
  // half a serial space is intentionally unordered in either direction.
  const uint16_t forwardDistance = static_cast<uint16_t>(rhs - lhs);
  return forwardDistance < 0x8000U;
}

struct WorkIdentity {
  uint16_t sourceToken;
  uint16_t sessionGeneration;

  WorkIdentity() : sourceToken(0U), sessionGeneration(0U) {}
  WorkIdentity(uint16_t token, uint16_t generation)
      : sourceToken(token), sessionGeneration(generation) {}

  bool valid() const {
    return sourceToken != 0U && sessionGeneration != 0U;
  }
};

inline bool sameWorkIdentity(const WorkIdentity& lhs,
                             const WorkIdentity& rhs) {
  return lhs.sourceToken == rhs.sourceToken &&
         lhs.sessionGeneration == rhs.sessionGeneration;
}

// Compare source order only within one session. Different generations are not
// ordered because teardown may leave old tokens numerically near new tokens.
inline bool workIdentityBefore(const WorkIdentity& lhs,
                               const WorkIdentity& rhs) {
  return lhs.valid() && rhs.valid() &&
         lhs.sessionGeneration == rhs.sessionGeneration &&
         nonZeroSerial16Before(lhs.sourceToken, rhs.sourceToken);
}

enum class SessionPhase : uint8_t {
  Closed = 0U,
  Teardown,
  Initializing,
  Ready,
};

// A borrowed snapshot of the caller-owned lifecycle state.
struct SessionStateView {
  bool active;
  uint16_t currentGeneration;
  uint16_t appliedGeneration;
  SessionPhase phase;

  SessionStateView()
      : active(false),
        currentGeneration(0U),
        appliedGeneration(0U),
        phase(SessionPhase::Closed) {}

  SessionStateView(bool requestedActive,
                   uint16_t current,
                   uint16_t applied,
                   SessionPhase currentPhase)
      : active(requestedActive),
        currentGeneration(current),
        appliedGeneration(applied),
        phase(currentPhase) {}
};

inline bool admissionOpen(const SessionStateView& session) {
  return session.active && session.currentGeneration != 0U &&
         session.currentGeneration == session.appliedGeneration &&
         session.phase == SessionPhase::Ready;
}

inline bool workCurrent(const WorkIdentity& work,
                        const SessionStateView& session) {
  return work.valid() && session.active &&
         work.sessionGeneration == session.currentGeneration &&
         session.currentGeneration == session.appliedGeneration &&
         session.phase == SessionPhase::Ready;
}

template<typename Value>
class ImmutableSnapshotView {
 public:
  ImmutableSnapshotView() : values_(nullptr), count_(0U) {}
  ImmutableSnapshotView(const Value* values, uint16_t count)
      : values_(values), count_(count) {}

  const Value* data() const { return values_; }
  uint16_t size() const { return count_; }

  bool validFor(uint16_t expectedCount) const {
    return expectedCount != 0U && values_ != nullptr &&
           count_ == expectedCount;
  }

 private:
  const Value* values_;
  uint16_t count_;
};

template<typename Value>
struct IngressWorkView {
  WorkIdentity identity;
  uint16_t start;
  uint16_t count;
  IngressDelivery delivery;
  ImmutableSnapshotView<Value> snapshot;

  IngressWorkView()
      : identity(),
        start(0U),
        count(0U),
        delivery(IngressDelivery::Rejected),
        snapshot() {}

  IngressWorkView(const WorkIdentity& workIdentity,
                  uint16_t upstreamStart,
                  uint16_t quantity,
                  IngressDelivery workDelivery,
                  ImmutableSnapshotView<Value> immutableSnapshot)
      : identity(workIdentity),
        start(upstreamStart),
        count(quantity),
        delivery(workDelivery),
        snapshot(immutableSnapshot) {}

  // Identity is deliberately not part of the payload/range shape. A simple
  // cooperative forwarder may already own ordering and use the planner with
  // the default zero identity. Session checks and CompletionAggregate require
  // a non-zero WorkIdentity when those facilities are used.
  bool validAcceptedShape() const {
    return isAccepted(delivery) && snapshot.validFor(count) &&
           static_cast<uint32_t>(start) + static_cast<uint32_t>(count) <=
               0x10000UL;
  }
};

typedef IngressWorkView<bool> CoilIngressWorkView;
typedef IngressWorkView<uint16_t> HoldingIngressWorkView;

inline CoilIngressWorkView makeCoilIngressWork(
    const WorkIdentity& identity,
    uint16_t start,
    uint16_t count,
    IngressDelivery delivery,
    const bool* snapshot) {
  return CoilIngressWorkView(
      identity, start, count, delivery,
      ImmutableSnapshotView<bool>(snapshot, count));
}

inline HoldingIngressWorkView makeHoldingIngressWork(
    const WorkIdentity& identity,
    uint16_t start,
    uint16_t count,
    IngressDelivery delivery,
    const uint16_t* snapshot) {
  return HoldingIngressWorkView(
      identity, start, count, delivery,
      ImmutableSnapshotView<uint16_t>(snapshot, count));
}

}  // namespace ModbusRTUBridge
