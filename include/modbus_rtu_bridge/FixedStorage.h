#pragma once

// Caller-owned, fixed-capacity FIFO storage.
//
// FixedRingQueue never invokes an allocator or constructs storage. The caller
// supplies an array of already-constructed T objects and keeps that array alive
// for the complete queue lifetime. Assigning T is still allowed to run T's own
// user-defined behavior, so use scalar/view records when allocation freedom is
// required. This makes capacity and RAM use visible at the integration boundary.

#include <stdint.h>

namespace ModbusRTUBridge {

template<typename T>
class FixedRingQueue {
 public:
  FixedRingQueue()
      : storage_(nullptr), capacity_(0U), head_(0U), size_(0U) {}

  FixedRingQueue(T* storage, uint16_t capacity)
      : storage_(storage), capacity_(capacity), head_(0U), size_(0U) {}

  // Copies would alias the same backing array while allowing head/size state
  // to diverge. Pass this small queue by reference instead.
  FixedRingQueue(const FixedRingQueue&) = delete;
  FixedRingQueue& operator=(const FixedRingQueue&) = delete;

  // A zero-capacity queue is a valid disabled queue. A non-zero capacity must
  // always have backing storage.
  bool validStorage() const {
    return capacity_ == 0U || storage_ != nullptr;
  }

  uint16_t capacity() const { return capacity_; }
  uint16_t size() const { return size_; }
  bool empty() const { return size_ == 0U; }
  bool full() const { return size_ == capacity_; }

  // Assignment publishes one complete record into the next FIFO slot. The
  // queue does not copy any payload referenced by T; pointed-to data retains
  // the lifetime requirements of the stored view.
  bool tryPush(const T& value) {
    if(!validStorage() || full()){
      return false;
    }
    storage_[physicalIndex(size_)] = value;
    ++size_;
    return true;
  }

  bool tryPop(T& valueOut) {
    if(empty() || !validStorage()){
      return false;
    }
    valueOut = storage_[head_];
    popFront();
    return true;
  }

  bool popFront() {
    if(empty() || !validStorage()){
      return false;
    }
    head_ = increment(head_);
    --size_;
    if(size_ == 0U){
      // Canonicalize the empty state. This is not required for correctness,
      // but makes reset/wrap traces deterministic for diagnostics and tests.
      head_ = 0U;
    }
    return true;
  }

  T* front() {
    return empty() || !validStorage() ? nullptr : storage_ + head_;
  }

  const T* front() const {
    return empty() || !validStorage() ? nullptr : storage_ + head_;
  }

  T* at(uint16_t logicalIndex) {
    return logicalIndex < size_ && validStorage()
        ? storage_ + physicalIndex(logicalIndex)
        : nullptr;
  }

  const T* at(uint16_t logicalIndex) const {
    return logicalIndex < size_ && validStorage()
        ? storage_ + physicalIndex(logicalIndex)
        : nullptr;
  }

  // Clear queue metadata only. Objects in caller storage stay constructed and
  // are overwritten by later pushes; no destructor or allocator is invoked.
  void clear() {
    head_ = 0U;
    size_ = 0U;
  }

 private:
  uint16_t increment(uint16_t index) const {
    return static_cast<uint16_t>(index + 1U) == capacity_
        ? 0U
        : static_cast<uint16_t>(index + 1U);
  }

  uint16_t physicalIndex(uint16_t logicalIndex) const {
    const uint32_t sum = static_cast<uint32_t>(head_) + logicalIndex;
    // Valid queue indices produce sum < 2 * capacity. Compare/subtract avoids
    // a runtime integer division on small targets such as AVR.
    return static_cast<uint16_t>(
        sum >= capacity_ ? sum - capacity_ : sum);
  }

  T* storage_;
  uint16_t capacity_;
  uint16_t head_;
  uint16_t size_;
};

}  // namespace ModbusRTUBridge
