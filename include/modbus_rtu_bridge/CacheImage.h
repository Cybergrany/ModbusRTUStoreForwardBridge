#pragma once

// Non-owning cached-image helpers for allocation-free store-forward bridges.
//
// CacheImage never locks. The product adapter retains ownership of memory and
// chooses the same lock scope it used before migration. Keeping synchronization
// outside this type avoids baking mbed, Arduino, RTOS, or OGM mutex concepts
// into the reusable core.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ModbusRTUBridge {

template<typename Value>
class MutableImageView {
 public:
  MutableImageView() : values_(nullptr), count_(0U) {}
  MutableImageView(Value* values, uint16_t count)
      : values_(values), count_(count) {}

  Value* data() const { return values_; }
  uint16_t size() const { return count_; }
  bool empty() const { return count_ == 0U; }

  bool contains(uint16_t start, uint16_t count) const {
    if(count == 0U){
      return false;
    }
    return values_ != nullptr &&
           static_cast<uint32_t>(start) + static_cast<uint32_t>(count) <=
               static_cast<uint32_t>(count_);
  }

  bool copyFrom(uint16_t start, const Value* source, uint16_t count) const {
    if(source == nullptr || !contains(start, count)){
      return false;
    }
    copyValues(values_ + start, source, count);
    return true;
  }

  bool copyTo(uint16_t start, Value* destination, uint16_t count) const {
    if(destination == nullptr || !contains(start, count)){
      return false;
    }
    copyValues(destination, values_ + start, count);
    return true;
  }

 private:
  static void copyValues(Value* destination,
                         const Value* source,
                         uint16_t count) {
    // The scalar case avoids assumptions about bool object representation in
    // toolchains that specialize or instrument memcpy. Multi-element copies
    // retain the established compact embedded implementation.
    if(count == 1U){
      destination[0] = source[0];
      return;
    }
    memcpy(destination, source, static_cast<size_t>(count) * sizeof(Value));
  }

  Value* values_;
  uint16_t count_;
};

// Visible is the flattened upstream image. Applied is the last state known to
// have been accepted/applied by the downstream side. Product policy decides
// when to advance Applied. On a locally failed strict write, restore() rolls
// Visible back to Applied using the exact same range.
template<typename Value>
class DesiredAppliedCache {
 public:
  DesiredAppliedCache() : visible_(), applied_() {}

  DesiredAppliedCache(MutableImageView<Value> visible,
                      MutableImageView<Value> applied)
      : visible_(visible), applied_(applied) {}

  const MutableImageView<Value>& visible() const { return visible_; }
  const MutableImageView<Value>& applied() const { return applied_; }

  bool validShape() const {
    return visible_.size() == applied_.size() &&
           (visible_.size() == 0U ||
            (visible_.data() != nullptr && applied_.data() != nullptr));
  }

  bool captureDesired(uint16_t start,
                      const Value* immutableSnapshot,
                      uint16_t count) const {
    return visible_.copyFrom(start, immutableSnapshot, count);
  }

  bool markApplied(uint16_t start,
                   const Value* immutableSnapshot,
                   uint16_t count) const {
    return applied_.copyFrom(start, immutableSnapshot, count);
  }

  bool restore(uint16_t start, uint16_t count) const {
    if(!visible_.contains(start, count) || !applied_.contains(start, count)){
      return false;
    }
    return visible_.copyFrom(start, applied_.data() + start, count);
  }

 private:
  MutableImageView<Value> visible_;
  MutableImageView<Value> applied_;
};

}  // namespace ModbusRTUBridge

