#pragma once

// Optional adapter for the common ModbusRTUMaster method surface.
//
// Include the concrete master's public header before instantiating this
// template. This file intentionally does not include ModbusRTUMaster.h and the
// package manifest does not depend on it, so users of another transport pay no
// dependency or compile-time cost.

#include <stdint.h>

#include <modbus_rtu_bridge/DownstreamExecutor.h>

namespace ModbusRTUBridge {

template<typename Result>
struct ModbusRTUMasterResultCodes {
  Result invalidQuantity;
  Result invalidBuffer;
  Result invalidOperation;

  ModbusRTUMasterResultCodes(Result quantity,
                            Result buffer,
                            Result operation)
      : invalidQuantity(quantity),
        invalidBuffer(buffer),
        invalidOperation(operation) {}
};

// Master must provide the eight conventional read/write methods used by
// ModbusRTUMaster plus getExceptionResponse(). Result is normally that
// master's error enum. Optional mutex/context parameters remain an application
// concern because their type is platform-specific; a custom backend can use
// DownstreamRequest::consistencyContext when it needs them.
template<typename Master, typename MasterResult>
class ModbusRTUMasterBackend {
 public:
  typedef MasterResult Result;

  ModbusRTUMasterBackend(
      Master& master,
      const ModbusRTUMasterResultCodes<Result>& invalidResults)
      : master_(&master), invalidResults_(invalidResults) {}

  Result invalidQuantityResult() const {
    return invalidResults_.invalidQuantity;
  }

  Result invalidBufferResult() const {
    return invalidResults_.invalidBuffer;
  }

  Result invalidOperationResult() const {
    return invalidResults_.invalidOperation;
  }

  uint8_t exceptionCode() const {
    return master_->getExceptionResponse();
  }

  Result readCoils(const DownstreamRequest& request) {
    return master_->readCoils(
        request.endpointId, request.startAddress,
        request.coilBuffer, request.quantity);
  }

  Result readDiscreteInputs(const DownstreamRequest& request) {
    return master_->readDiscreteInputs(
        request.endpointId, request.startAddress,
        request.coilBuffer, request.quantity);
  }

  Result readHoldingRegisters(const DownstreamRequest& request) {
    return master_->readHoldingRegisters(
        request.endpointId, request.startAddress,
        request.registerBuffer, request.quantity);
  }

  Result readInputRegisters(const DownstreamRequest& request) {
    return master_->readInputRegisters(
        request.endpointId, request.startAddress,
        request.registerBuffer, request.quantity);
  }

  Result writeSingleCoil(const DownstreamRequest& request) {
    return master_->writeSingleCoil(
        request.endpointId, request.startAddress, request.coilValue);
  }

  Result writeSingleHoldingRegister(const DownstreamRequest& request) {
    return master_->writeSingleHoldingRegister(
        request.endpointId, request.startAddress, request.registerValue);
  }

  Result writeMultipleCoils(const DownstreamRequest& request) {
    return master_->writeMultipleCoils(
        request.endpointId, request.startAddress,
        request.coilBuffer, request.quantity);
  }

  Result writeMultipleHoldingRegisters(const DownstreamRequest& request) {
    return master_->writeMultipleHoldingRegisters(
        request.endpointId, request.startAddress,
        request.registerBuffer, request.quantity);
  }

 private:
  Master* master_;
  ModbusRTUMasterResultCodes<Result> invalidResults_;
};

}  // namespace ModbusRTUBridge
