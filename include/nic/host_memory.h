#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace nic {

using HostAddress = std::uint64_t;

/// Configuration for host memory emulation.
struct HostMemoryConfig {
  std::size_t size_bytes{0};
  std::size_t page_size{4096};
  bool iommu_enabled{false};
};

/// Error codes for host memory accesses.
enum class HostMemoryError : std::uint8_t {
  None,
  OutOfBounds,
  IommuFault,
  FaultInjected,
};

/// Result of a host memory operation.
struct HostMemoryResult {
  HostMemoryError error{HostMemoryError::None};
  std::size_t bytes_processed{0};

  [[nodiscard]] bool ok() const noexcept { return error == HostMemoryError::None; }
};

/// Mutable view into translated host memory.
struct HostMemoryView {
  std::byte* data{nullptr};
  std::size_t length{0};
  HostAddress address{0};
};

/// Const view into translated host memory.
struct ConstHostMemoryView {
  const std::byte* data{nullptr};
  std::size_t length{0};
  HostAddress address{0};
};

/// Interface for host memory access and address translation.
class HostMemory {
public:
  virtual ~HostMemory() = default;

  /// Return the configuration for this host memory instance.
  [[nodiscard]] virtual HostMemoryConfig config() const noexcept = 0;

  /// Translate an address/length to a mutable view, honoring IOMMU rules.
  [[nodiscard]] virtual HostMemoryResult translate(HostAddress address,
                                                   std::size_t length,
                                                   HostMemoryView& view) = 0;

  /// Translate an address/length to a const view, honoring IOMMU rules.
  [[nodiscard]] virtual HostMemoryResult translate_const(HostAddress address,
                                                         std::size_t length,
                                                         ConstHostMemoryView& view) const = 0;

  /// Read from host memory into the provided buffer.
  [[nodiscard]] virtual HostMemoryResult read(HostAddress address,
                                              std::span<std::byte> buffer) const = 0;

  /// Write data into host memory.
  [[nodiscard]] virtual HostMemoryResult write(HostAddress address,
                                               std::span<const std::byte> data) = 0;
};

}  // namespace nic
