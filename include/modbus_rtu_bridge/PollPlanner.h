#pragma once

// Pure downstream poll/forward fairness planning.
//
// select() reads caller-supplied time and candidate state without modifying
// anything. commit() advances fairness state only after the caller accepts the
// selected phase. Acceptance normally means queue admission; it may instead be
// an intentional policy-gated/no-dispatch poll consumption. A full/rejected
// queue omits commit(), so the same due poll remains selectable.

#include <stdint.h>

#include <modbus_rtu_bridge/DownstreamExecutor.h>

namespace ModbusRTUBridge {

// Half-range modular deadline comparison. now and dueAt must describe events
// less than 2^31 ticks apart; the tick unit (microseconds, milliseconds, loop
// epochs, and so on) belongs entirely to the caller.
inline bool pollDeadlineReached32(uint32_t now, uint32_t dueAt) {
  return static_cast<uint32_t>(now - dueAt) < 0x80000000UL;
}

struct PollCandidate {
  DownstreamRequest request;
  uint32_t dueAt;
  bool enabled;
  // Some policies consume a poll phase without putting a frame on the wire
  // (for example, an adaptively gated endpoint). Such a phase still advances
  // round-robin/last-poll state when committed, but needs no valid request.
  bool requiresDispatch;

  PollCandidate()
      : request(), dueAt(0UL), enabled(false), requiresDispatch(true) {}

  PollCandidate(const DownstreamRequest& pollRequest,
                uint32_t deadline,
                bool pollEnabled = true,
                bool dispatchRequired = true)
      : request(pollRequest),
        dueAt(deadline),
        enabled(pollEnabled),
        requiresDispatch(dispatchRequired) {}
};

enum class ScheduledActionKind : uint8_t {
  None = 0U,
  ForwardWrite,
  Poll,
};

struct PollSelection {
  ScheduledActionKind kind;
  uint16_t pollIndex;
  // Selections returned by PollPlanner are bound to the candidate-set size
  // used to make the fairness decision. This prevents a caller from selecting
  // against one set and committing against another after an array resize.
  // The two-argument constructor remains deliberately unbound for callers
  // that manually construct a commit after applying their own selection
  // policy.
  uint16_t originatingPollCount;
  bool pollCountBound;

  PollSelection()
      : kind(ScheduledActionKind::None),
        pollIndex(0U),
        originatingPollCount(0U),
        pollCountBound(false) {}

  PollSelection(ScheduledActionKind selectedKind, uint16_t selectedPoll)
      : kind(selectedKind),
        pollIndex(selectedPoll),
        originatingPollCount(0U),
        pollCountBound(false) {}

  static PollSelection fromCandidateSet(ScheduledActionKind selectedKind,
                                        uint16_t selectedPoll,
                                        uint16_t pollCount) {
    PollSelection selection(selectedKind, selectedPoll);
    selection.originatingPollCount = pollCount;
    selection.pollCountBound = true;
    return selection;
  }
};

struct PollPlannerState {
  uint16_t nextPollIndex;
  uint16_t forwardsSincePoll;
  uint32_t lastPollAt;
  bool hasConsumedPoll;

  PollPlannerState()
      : nextPollIndex(0U),
        forwardsSincePoll(0U),
        lastPollAt(0UL),
        hasConsumedPoll(false) {}
};

// Transient, caller-owned scan state for one handler pass. Advancing this
// cursor does not consume a poll globally. It lets application policy decline
// one due candidate and inspect later candidates without either committing the
// declined phase or reselecting it in a loop.
struct PollScanCursor {
  uint16_t nextIndex;
  uint16_t remaining;
  uint16_t candidateCount;
  bool active;

  PollScanCursor()
      : nextIndex(0U),
        remaining(0U),
        candidateCount(0U),
        active(false) {}
};

enum class PollScanIndexStatus : uint8_t {
  Candidate = 0U,
  Complete,
};

enum class PollDueStatus : uint8_t {
  Due = 0U,
  Complete,
  InvalidStorage,
  CandidateCountChanged,
};

struct PollPlannerOptions {
  // Once a due poll exists, no more than this many forward writes may be
  // admitted since the last admitted poll. Zero gives due polls immediate
  // priority; 0xFFFF permits the longest representable forward burst.
  uint16_t maxConsecutiveForwards;

  PollPlannerOptions() : maxConsecutiveForwards(1U) {}
  explicit PollPlannerOptions(uint16_t maximumForwardBurst)
      : maxConsecutiveForwards(maximumForwardBurst) {}
};

enum class PollSelectStatus : uint8_t {
  Selected = 0U,
  NoAction,
  InvalidStorage,
};

class PollPlanner {
 public:
  PollPlanner() : options_() {}
  explicit PollPlanner(const PollPlannerOptions& options) : options_(options) {}

  const PollPlannerOptions& options() const { return options_; }

  void beginPollScan(uint16_t pollCount,
                     const PollPlannerState& state,
                     PollScanCursor& cursor) const {
    cursor = PollScanCursor();
    if(pollCount == 0U){
      return;
    }
    cursor.nextIndex = state.nextPollIndex < pollCount
        ? state.nextPollIndex
        : 0U;
    cursor.remaining = pollCount;
    cursor.candidateCount = pollCount;
    cursor.active = true;
  }

  // Yield round-robin indices without requiring a PollCandidate array. A hot
  // path can inspect one endpoint's existing due/enabled state, decline it,
  // then request the next index. This transient scan never changes persistent
  // fairness state and stores only three uint16_t values plus a flag.
  PollScanIndexStatus nextIndex(PollScanCursor& cursor,
                                uint16_t& indexOut) const {
    if(!cursor.active || cursor.remaining == 0U ||
       cursor.candidateCount == 0U){
      cursor.active = false;
      return PollScanIndexStatus::Complete;
    }
    indexOut = cursor.nextIndex;
    ++cursor.nextIndex;
    if(cursor.nextIndex == cursor.candidateCount){
      cursor.nextIndex = 0U;
    }
    --cursor.remaining;
    if(cursor.remaining == 0U){
      cursor.active = false;
    }
    return PollScanIndexStatus::Candidate;
  }

  // Convenience scan for callers that already keep a PollCandidate array.
  // Low-RAM integrations can use nextIndex() above and avoid this array.
  PollDueStatus nextDue(uint32_t now,
                        const PollCandidate* polls,
                        uint16_t pollCount,
                        PollScanCursor& cursor,
                        PollSelection& selection) const {
    selection = PollSelection();
    if(pollCount != 0U && polls == nullptr){
      cursor = PollScanCursor();
      return PollDueStatus::InvalidStorage;
    }
    if(cursor.candidateCount != pollCount){
      cursor = PollScanCursor();
      return PollDueStatus::CandidateCountChanged;
    }

    uint16_t index = 0U;
    while(nextIndex(cursor, index) == PollScanIndexStatus::Candidate){
      if(polls[index].enabled &&
         pollDeadlineReached32(now, polls[index].dueAt)){
        selection = PollSelection::fromCandidateSet(
            ScheduledActionKind::Poll, index, pollCount);
        return PollDueStatus::Due;
      }
    }
    return PollDueStatus::Complete;
  }

  PollSelectStatus select(uint32_t now,
                          bool forwardReady,
                          const PollCandidate* polls,
                          uint16_t pollCount,
                          const PollPlannerState& state,
                          PollSelection& selection) const {
    selection = PollSelection();
    if(pollCount != 0U && polls == nullptr){
      return PollSelectStatus::InvalidStorage;
    }

    PollScanCursor scan;
    beginPollScan(pollCount, state, scan);
    PollSelection dueSelection;
    const PollDueStatus scanStatus = nextDue(
        now, polls, pollCount, scan, dueSelection);
    if(scanStatus == PollDueStatus::InvalidStorage ||
       scanStatus == PollDueStatus::CandidateCountChanged){
      return PollSelectStatus::InvalidStorage;
    }
    const bool due = scanStatus == PollDueStatus::Due;
    if(forwardReady &&
       (!due || state.forwardsSincePoll < options_.maxConsecutiveForwards)){
      selection = PollSelection::fromCandidateSet(
          ScheduledActionKind::ForwardWrite, 0U, pollCount);
      return PollSelectStatus::Selected;
    }
    if(due){
      selection = dueSelection;
      return PollSelectStatus::Selected;
    }
    if(forwardReady){
      selection = PollSelection::fromCandidateSet(
          ScheduledActionKind::ForwardWrite, 0U, pollCount);
      return PollSelectStatus::Selected;
    }
    return PollSelectStatus::NoAction;
  }

  // Commit is deliberately separate from select. Call it after a forwarding
  // request is admitted, after a poll request is admitted, or after application
  // policy deliberately consumes a no-dispatch poll phase. Do not call it for
  // queue rejection. After a poll commit, refresh that candidate's dueAt or
  // enabled state before selecting again; the planner imposes no global
  // one-poll-per-timestamp rule. Planner-produced selections are bound to the
  // selection-time pollCount and reject a commit with a changed count.
  bool commit(const PollSelection& selection,
              uint16_t pollCount,
              uint32_t now,
              PollPlannerState& state) const {
    if(selection.pollCountBound &&
       selection.originatingPollCount != pollCount){
      return false;
    }
    switch(selection.kind){
      case ScheduledActionKind::ForwardWrite:
        if(state.forwardsSincePoll != 0xFFFFU){
          ++state.forwardsSincePoll;
        }
        return true;
      case ScheduledActionKind::Poll:
        if(pollCount == 0U || selection.pollIndex >= pollCount){
          return false;
        }
        state.nextPollIndex = static_cast<uint16_t>(selection.pollIndex + 1U);
        if(state.nextPollIndex == pollCount){
          state.nextPollIndex = 0U;
        }
        state.forwardsSincePoll = 0U;
        state.lastPollAt = now;
        state.hasConsumedPoll = true;
        return true;
      case ScheduledActionKind::None:
      default:
        return false;
    }
  }

 private:
  PollPlannerOptions options_;
};

}  // namespace ModbusRTUBridge
