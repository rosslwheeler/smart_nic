#include "nic/dma_types.h"

#include "nic/host_memory.h"
#include "nic/trace.h"

using namespace nic;

DmaError nic::ToDmaError(HostMemoryError error) noexcept {
  NIC_TRACE_SCOPED(__func__);
  switch (error) {
    case HostMemoryError::None:
      return DmaError::None;
    case HostMemoryError::OutOfBounds:
      return DmaError::OutOfBounds;
    case HostMemoryError::IommuFault:
      return DmaError::TranslationFault;
    case HostMemoryError::FaultInjected:
      return DmaError::FaultInjected;
  }
  return DmaError::InternalError;
}

void nic::trace_dma_error(DmaError error, const char* context) {
  NIC_TRACE_SCOPED(__func__);
  if (error == DmaError::None) {
    return;
  }

  const char* msg = "dma_error";
  if (context != nullptr) {
    msg = context;
  }
  trace::message(msg);
}
