#pragma once

#include <cstddef>
#include <cstdint>

namespace nic {

enum class DmaError : std::uint8_t {
  None,
  TranslationFault,
  OutOfBounds,
  FaultInjected,
  AccessError,
  AlignmentError,
  InternalError,
};

struct DmaResult {
  DmaError error{DmaError::None};
  std::size_t bytes_processed{0};
  const char* message{nullptr};

  [[nodiscard]] bool ok() const noexcept { return error == DmaError::None; }
};

enum class DmaDirection : std::uint8_t { Read, Write };

class HostMemory;
enum class HostMemoryError : std::uint8_t;

/// Map host memory errors to DMA-facing errors.
DmaError ToDmaError(HostMemoryError error) noexcept;

/// Emit a trace marker for a DMA error (no-op for DmaError::None).
void trace_dma_error(DmaError error, const char* context = nullptr);

}  // namespace nic
