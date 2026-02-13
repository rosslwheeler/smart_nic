#pragma once

#include <array>
#include <cstdint>

#include "nic/pcie_formats.h"

namespace nic {

/// BAR type per PCIe specification.
enum class BarType : std::uint8_t {
  Memory32 = 0,  ///< 32-bit memory-mapped BAR
  Memory64 = 1,  ///< 64-bit memory-mapped BAR (consumes two BAR slots)
  IO = 2,        ///< I/O space BAR (legacy, rarely used)
  Disabled = 3,  ///< BAR is not configured
};

/// Base Address Register configuration.
struct Bar {
  std::uint64_t base_address{0};  ///< Base address (host physical or IOVA)
  std::uint64_t size{0};          ///< Size in bytes (must be power of 2)
  BarType type{BarType::Disabled};
  bool prefetchable{false};  ///< Memory is prefetchable (no side effects)

  [[nodiscard]] bool is_enabled() const noexcept { return type != BarType::Disabled && size > 0; }

  [[nodiscard]] bool is_memory() const noexcept {
    return type == BarType::Memory32 || type == BarType::Memory64;
  }

  [[nodiscard]] bool is_64bit() const noexcept { return type == BarType::Memory64; }

  /// Return the BAR type field value for memory BAR encoding (bits 1-2).
  [[nodiscard]] std::uint64_t bar_type_field() const noexcept {
    return is_64bit() ? pcie::bar_type::k64Bit : pcie::bar_type::k32Bit;
  }

  /// Extract the address field for memory BAR encoding (bits 4-31, 28 bits).
  /// Memory BARs are 16-byte aligned minimum, so bits 0-3 encode type/prefetchable.
  [[nodiscard]] std::uint64_t memory_bar_address_field() const noexcept {
    return (base_address >> 4) & 0x0FFFFFFF;
  }

  /// Extract the address field for I/O BAR encoding (bits 2-31, 30 bits).
  /// I/O BARs are 4-byte aligned, so bits 0-1 encode the I/O space indicator.
  [[nodiscard]] std::uint64_t io_bar_address_field() const noexcept {
    return (base_address >> 2) & 0x3FFFFFFF;
  }

  /// Extract upper 32 bits for 64-bit BAR encoding.
  [[nodiscard]] std::uint32_t upper_address_dword() const noexcept {
    return static_cast<std::uint32_t>(base_address >> 32);
  }
};

/// PCIe devices have up to 6 BARs (BAR0-BAR5).
/// Note: A 64-bit BAR consumes two consecutive BAR slots.
inline constexpr std::size_t kMaxBars = 6;

using BarArray = std::array<Bar, kMaxBars>;

/// Create default BAR configuration for a typical NIC.
/// Configures BAR0 as 64-bit MMIO for registers and BAR2 as 64-bit MMIO for
/// doorbells.
inline BarArray MakeDefaultBars() {
  BarArray bars{};

  // BAR0: Main register space (64-bit, 64KB)
  bars[0] = Bar{
      .base_address = 0,
      .size = 64 * 1024,
      .type = BarType::Memory64,
      .prefetchable = false,
  };
  // BAR1 is consumed by BAR0's 64-bit address

  // BAR2: Doorbell/notification region (64-bit, 16KB)
  bars[2] = Bar{
      .base_address = 0,
      .size = 16 * 1024,
      .type = BarType::Memory64,
      .prefetchable = false,
  };
  // BAR3 is consumed by BAR2's 64-bit address

  // BAR4, BAR5: Reserved/disabled for now

  return bars;
}

}  // namespace nic
