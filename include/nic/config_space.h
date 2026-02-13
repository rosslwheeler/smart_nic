#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "bit_fields/bit_fields.h"
#include "nic/bar.h"
#include "nic/capability.h"
#include "nic/pcie_formats.h"

namespace nic {

/// Read a little-endian value from a byte array using bit_fields.
template <typename T>
  requires std::is_unsigned_v<T>
[[nodiscard]] inline T read_le(const std::uint8_t* data) noexcept {
  auto buffer = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), sizeof(T));
  bit_fields::BitReader<bit_fields::WireOrder::LittleEndian> reader(buffer);
  return reader.read_aligned<T>();
}

/// Write a value to a byte array in little-endian format using bit_fields.
template <typename T>
  requires std::is_unsigned_v<T>
inline void write_le(std::uint8_t* data, T value) noexcept {
  auto buffer = std::span<std::byte>(reinterpret_cast<std::byte*>(data), sizeof(T));
  bit_fields::BitWriter<bit_fields::WireOrder::LittleEndian> writer(buffer);
  writer.write_aligned(value);
}

/// PCIe configuration space size (4KB for extended config space).
inline constexpr std::size_t kConfigSpaceSize = 4096;

/// Standard PCIe configuration header offsets.
namespace config_offset {

// Type 0 (Endpoint) header - common fields
inline constexpr std::uint16_t kVendorId = 0x00;
inline constexpr std::uint16_t kDeviceId = 0x02;
inline constexpr std::uint16_t kCommand = 0x04;
inline constexpr std::uint16_t kStatus = 0x06;
inline constexpr std::uint16_t kRevisionId = 0x08;
inline constexpr std::uint16_t kClassCode = 0x09;  // 3 bytes: ProgIF, Subclass, Class
inline constexpr std::uint16_t kCacheLineSize = 0x0C;
inline constexpr std::uint16_t kLatencyTimer = 0x0D;
inline constexpr std::uint16_t kHeaderType = 0x0E;
inline constexpr std::uint16_t kBIST = 0x0F;

// BAR registers (Type 0)
inline constexpr std::uint16_t kBar0 = 0x10;
inline constexpr std::uint16_t kBar1 = 0x14;
inline constexpr std::uint16_t kBar2 = 0x18;
inline constexpr std::uint16_t kBar3 = 0x1C;
inline constexpr std::uint16_t kBar4 = 0x20;
inline constexpr std::uint16_t kBar5 = 0x24;

inline constexpr std::uint16_t kCardbusCISPtr = 0x28;
inline constexpr std::uint16_t kSubsystemVendorId = 0x2C;
inline constexpr std::uint16_t kSubsystemId = 0x2E;
inline constexpr std::uint16_t kExpansionROMBase = 0x30;
inline constexpr std::uint16_t kCapabilitiesPtr = 0x34;
inline constexpr std::uint16_t kInterruptLine = 0x3C;
inline constexpr std::uint16_t kInterruptPin = 0x3D;
inline constexpr std::uint16_t kMinGrant = 0x3E;
inline constexpr std::uint16_t kMaxLatency = 0x3F;

// Extended config space starts at 0x100
inline constexpr std::uint16_t kExtendedConfigBase = 0x100;

// Array of all BAR offsets for iteration
inline constexpr std::array<std::uint16_t, 6> kBarOffsets = {
    kBar0,
    kBar1,
    kBar2,
    kBar3,
    kBar4,
    kBar5,
};

}  // namespace config_offset

/// Command register bits.
namespace command_bits {
inline constexpr std::uint16_t kIOSpace = 1 << 0;
inline constexpr std::uint16_t kMemorySpace = 1 << 1;
inline constexpr std::uint16_t kBusMaster = 1 << 2;
inline constexpr std::uint16_t kSpecialCycles = 1 << 3;
inline constexpr std::uint16_t kMemWriteInvalidate = 1 << 4;
inline constexpr std::uint16_t kVGAPaletteSnoop = 1 << 5;
inline constexpr std::uint16_t kParityErrorResponse = 1 << 6;
inline constexpr std::uint16_t kSERREnabled = 1 << 8;
inline constexpr std::uint16_t kFastB2BEnabled = 1 << 9;
inline constexpr std::uint16_t kInterruptDisable = 1 << 10;
}  // namespace command_bits

/// Status register bits.
namespace status_bits {
inline constexpr std::uint16_t kInterruptStatus = 1 << 3;
inline constexpr std::uint16_t kCapabilitiesList = 1 << 4;
inline constexpr std::uint16_t k66MHzCapable = 1 << 5;
inline constexpr std::uint16_t kFastB2BCapable = 1 << 7;
inline constexpr std::uint16_t kMasterDataParityError = 1 << 8;
inline constexpr std::uint16_t kSignaledTargetAbort = 1 << 11;
inline constexpr std::uint16_t kReceivedTargetAbort = 1 << 12;
inline constexpr std::uint16_t kReceivedMasterAbort = 1 << 13;
inline constexpr std::uint16_t kSignaledSystemError = 1 << 14;
inline constexpr std::uint16_t kDetectedParityError = 1 << 15;
}  // namespace status_bits

/// PCI class codes for Network Controller.
namespace class_code {
inline constexpr std::uint8_t kNetworkController = 0x02;
inline constexpr std::uint8_t kEthernetController = 0x00;
}  // namespace class_code

/// PCIe configuration space abstraction.
/// Provides typed access to the standard 4KB config space.
class ConfigSpace {
public:
  ConfigSpace() { data_.fill(0); }

  /// Initialize config space with device identity and capabilities.
  void initialize(std::uint16_t vendor_id,
                  std::uint16_t device_id,
                  std::uint8_t revision,
                  const BarArray& bars,
                  const CapabilityList& caps);

  /// Read 8-bit value from config space.
  [[nodiscard]] std::uint8_t read8(std::uint16_t offset) const noexcept {
    if (offset >= kConfigSpaceSize) {
      return 0xFF;
    }
    return data_[offset];
  }

  /// Read 16-bit value from config space (little-endian).
  [[nodiscard]] std::uint16_t read16(std::uint16_t offset) const noexcept {
    if (static_cast<std::size_t>(offset) + 1 >= kConfigSpaceSize) {
      return 0xFFFF;
    }
    return read_le<std::uint16_t>(&data_[offset]);
  }

  /// Read 32-bit value from config space (little-endian).
  [[nodiscard]] std::uint32_t read32(std::uint16_t offset) const noexcept {
    if (static_cast<std::size_t>(offset) + 3 >= kConfigSpaceSize) {
      return 0xFFFFFFFF;
    }
    return read_le<std::uint32_t>(&data_[offset]);
  }

  /// Write 8-bit value to config space.
  void write8(std::uint16_t offset, std::uint8_t value) noexcept {
    if (offset >= kConfigSpaceSize) {
      return;
    }
    if (is_read_only(offset)) {
      return;
    }
    data_[offset] = value;
  }

  /// Write 16-bit value to config space (little-endian).
  void write16(std::uint16_t offset, std::uint16_t value) noexcept {
    if (static_cast<std::size_t>(offset) + 1 >= kConfigSpaceSize) {
      return;
    }
    if (is_read_only(offset)) {
      return;
    }
    write_le(&data_[offset], value);
  }

  /// Write 32-bit value to config space (little-endian).
  void write32(std::uint16_t offset, std::uint32_t value) noexcept {
    if (static_cast<std::size_t>(offset) + 3 >= kConfigSpaceSize) {
      return;
    }
    if (is_read_only(offset)) {
      return;
    }
    write_le(&data_[offset], value);
  }

  /// Direct access to raw config space data.
  [[nodiscard]] const std::array<std::uint8_t, kConfigSpaceSize>& data() const noexcept {
    return data_;
  }

  // ==========================================================================
  // Field-level access using bit_fields formats
  // ==========================================================================

  /// Check if a specific command register bit is set.
  [[nodiscard]] bool is_command_bit_set(std::string_view field_name) const noexcept;

  /// Set a specific command register bit.
  void set_command_bit(std::string_view field_name, bool value) noexcept;

  /// Get a field value from the status register.
  [[nodiscard]] std::uint64_t get_status_field(std::string_view field_name) const noexcept;

private:
  std::array<std::uint8_t, kConfigSpaceSize> data_;

  /// Raw write methods that bypass read-only checks (for initialization only).
  void raw_write8(std::uint16_t offset, std::uint8_t value) noexcept {
    if (offset >= kConfigSpaceSize) {
      return;
    }
    data_[offset] = value;
  }

  void raw_write16(std::uint16_t offset, std::uint16_t value) noexcept {
    if (static_cast<std::size_t>(offset) + 1 >= kConfigSpaceSize) {
      return;
    }
    write_le(&data_[offset], value);
  }

  void raw_write32(std::uint16_t offset, std::uint32_t value) noexcept {
    if (static_cast<std::size_t>(offset) + 3 >= kConfigSpaceSize) {
      return;
    }
    write_le(&data_[offset], value);
  }

  void initialize_bars(const BarArray& bars);
  void initialize_capabilities(const CapabilityList& caps);

  /// Check if offset is read-only (simplified check).
  [[nodiscard]] bool is_read_only(std::uint16_t offset) const noexcept;
};

}  // namespace nic
