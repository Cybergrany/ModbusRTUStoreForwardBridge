#pragma once

// Typed, platform-neutral downstream request execution.
//
// The executor is deliberately synchronous and stateless. A product runner may
// invoke it directly or from its existing bounded queue/worker arrangement.
// Retry policy, deadlines, queueing, clocks, logging and completion delivery
// remain outside this header. That boundary lets an mbed runner preserve exact
// scheduling while native tests use a deterministic fake backend.

#include <stdint.h>

namespace ModbusRTUBridge {

enum class DownstreamOperation : uint8_t {
  ReadCoils = 0U,
  ReadDiscreteInputs = 1U,
  ReadHoldingRegisters = 2U,
  ReadInputRegisters = 3U,
  WriteSingleCoil = 4U,
  WriteSingleHoldingRegister = 5U,
  WriteMultipleCoils = 6U,
  WriteMultipleHoldingRegisters = 7U,
};

struct DownstreamRequest {
  uint32_t sequence;
  DownstreamOperation operation;
  uint8_t endpointId;
  uint16_t startAddress;
  uint16_t quantity;
  bool* coilBuffer;
  uint16_t* registerBuffer;
  bool coilValue;
  uint16_t registerValue;
  void* consistencyContext;

  DownstreamRequest()
      : sequence(0U),
        operation(DownstreamOperation::ReadCoils),
        endpointId(0U),
        startAddress(0U),
        quantity(0U),
        coilBuffer(nullptr),
        registerBuffer(nullptr),
        coilValue(false),
        registerValue(0U),
        consistencyContext(nullptr) {}
};

enum class DownstreamRequestStatus : uint8_t {
  Valid = 0U,
  InvalidQuantity,
  InvalidBuffer,
  InvalidOperation,
};

inline DownstreamRequestStatus validateDownstreamRequest(
    const DownstreamRequest& request) {
  switch(request.operation){
    case DownstreamOperation::ReadCoils:
    case DownstreamOperation::ReadDiscreteInputs:
    case DownstreamOperation::WriteMultipleCoils:
      if(request.quantity == 0U){
        return DownstreamRequestStatus::InvalidQuantity;
      }
      return request.coilBuffer != nullptr
          ? DownstreamRequestStatus::Valid
          : DownstreamRequestStatus::InvalidBuffer;

    case DownstreamOperation::ReadHoldingRegisters:
    case DownstreamOperation::ReadInputRegisters:
    case DownstreamOperation::WriteMultipleHoldingRegisters:
      if(request.quantity == 0U){
        return DownstreamRequestStatus::InvalidQuantity;
      }
      return request.registerBuffer != nullptr
          ? DownstreamRequestStatus::Valid
          : DownstreamRequestStatus::InvalidBuffer;

    case DownstreamOperation::WriteSingleCoil:
    case DownstreamOperation::WriteSingleHoldingRegister:
      return DownstreamRequestStatus::Valid;
  }
  return DownstreamRequestStatus::InvalidOperation;
}

template<typename Result>
struct DownstreamCompletion {
  uint32_t sequence;
  Result result;
  uint8_t exceptionCode;

  DownstreamCompletion(uint32_t completionSequence,
                       Result completionResult,
                       uint8_t completionException)
      : sequence(completionSequence),
        result(completionResult),
        exceptionCode(completionException) {}
};

// Backend contract
// ----------------
// Backend must expose:
//   using Result = ...;
//   Result invalidQuantityResult() const;
//   Result invalidBufferResult() const;
//   Result invalidOperationResult() const;
//   uint8_t exceptionCode() const;
//   Result readCoils/readDiscreteInputs/readHoldingRegisters/
//          readInputRegisters(const DownstreamRequest&);
//   Result writeSingleCoil/writeSingleHoldingRegister/
//          writeMultipleCoils/writeMultipleHoldingRegisters(
//              const DownstreamRequest&);
//
// Passing the complete request lets a backend interpret consistencyContext as
// its native mutex/copy policy without exposing that platform type here.
template<typename Backend>
DownstreamCompletion<typename Backend::Result> executeValidatedDownstreamRequest(
    Backend& backend,
    const DownstreamRequest& request) {
  typedef typename Backend::Result Result;
  Result result = backend.invalidOperationResult();
  switch(request.operation){
    case DownstreamOperation::ReadCoils:
      result = backend.readCoils(request);
      break;
    case DownstreamOperation::ReadDiscreteInputs:
      result = backend.readDiscreteInputs(request);
      break;
    case DownstreamOperation::ReadHoldingRegisters:
      result = backend.readHoldingRegisters(request);
      break;
    case DownstreamOperation::ReadInputRegisters:
      result = backend.readInputRegisters(request);
      break;
    case DownstreamOperation::WriteSingleCoil:
      result = backend.writeSingleCoil(request);
      break;
    case DownstreamOperation::WriteSingleHoldingRegister:
      result = backend.writeSingleHoldingRegister(request);
      break;
    case DownstreamOperation::WriteMultipleCoils:
      result = backend.writeMultipleCoils(request);
      break;
    case DownstreamOperation::WriteMultipleHoldingRegisters:
      result = backend.writeMultipleHoldingRegisters(request);
      break;
  }

  return DownstreamCompletion<Result>(
      request.sequence, result, backend.exceptionCode());
}

template<typename Backend>
DownstreamCompletion<typename Backend::Result> executeDownstreamRequest(
    Backend& backend,
    const DownstreamRequest& request) {
  typedef typename Backend::Result Result;
  const DownstreamRequestStatus validation =
      validateDownstreamRequest(request);
  if(validation == DownstreamRequestStatus::InvalidQuantity){
    return DownstreamCompletion<Result>(
        request.sequence, backend.invalidQuantityResult(), 0U);
  }
  if(validation == DownstreamRequestStatus::InvalidBuffer){
    return DownstreamCompletion<Result>(
        request.sequence, backend.invalidBufferResult(), 0U);
  }
  if(validation != DownstreamRequestStatus::Valid){
    return DownstreamCompletion<Result>(
        request.sequence, backend.invalidOperationResult(), 0U);
  }
  return executeValidatedDownstreamRequest(backend, request);
}

}  // namespace ModbusRTUBridge
