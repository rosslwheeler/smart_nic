#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nic {

/// Register access type.
enum class RegisterAccess : std::uint8_t {
  RO,    ///< Read-only
  RW,    ///< Read-write
  RW1C,  ///< Read, write-1-to-clear
  RW1S,  ///< Read, write-1-to-set
  WO,    ///< Write-only (reads return 0)
  RC,    ///< Read-to-clear
};

/// Register width in bits.
enum class RegisterWidth : std::uint8_t {
  Bits8 = 8,
  Bits16 = 16,
  Bits32 = 32,
  Bits64 = 64,
};

/// Definition of a single register.
struct RegisterDef {
  std::string name;
  std::uint32_t offset;
  RegisterWidth width{RegisterWidth::Bits32};
  RegisterAccess access{RegisterAccess::RW};
  std::uint64_t reset_value{0};
  std::uint64_t write_mask{0xFFFFFFFF};  ///< Bits that can be written
};

/// Callback type for register side effects.
using RegisterCallback =
    std::function<void(std::uint32_t offset, std::uint64_t old_value, std::uint64_t new_value)>;

/// Register file abstraction for device MMIO regions.
/// Manages register definitions, values, and access semantics.
class RegisterFile {
public:
  RegisterFile() = default;

  /// Add a register definition.
  void add_register(RegisterDef register_definition) {
    registers_[register_definition.offset] = std::move(register_definition);
  }

  /// Add multiple register definitions.
  void add_registers(std::vector<RegisterDef> register_definitions) {
    for (auto& register_definition : register_definitions) {
      registers_[register_definition.offset] = std::move(register_definition);
    }
  }

  /// Reset all registers to their default values.
  void reset() {
    values_.clear();
    for (const auto& [offset, register_definition] : registers_) {
      values_[offset] = register_definition.reset_value;
    }
  }

  /// Read 32-bit register value.
  [[nodiscard]] std::uint32_t read32(std::uint32_t offset) const noexcept;

  /// Read 64-bit register value.
  [[nodiscard]] std::uint64_t read64(std::uint32_t offset) const noexcept;

  /// Write 32-bit value to register.
  void write32(std::uint32_t offset, std::uint32_t value);

  /// Write 64-bit value to register.
  void write64(std::uint32_t offset, std::uint64_t value);

  /// Set callback for register writes (for side effects).
  void set_write_callback(RegisterCallback callback) { write_callback_ = std::move(callback); }

  /// Check if a register is defined at the given offset.
  [[nodiscard]] bool has_register(std::uint32_t offset) const noexcept {
    return registers_.find(offset) != registers_.end();
  }

  /// Get register definition (for inspection/debugging).
  [[nodiscard]] const RegisterDef* get_register_def(std::uint32_t offset) const noexcept {
    auto it = registers_.find(offset);
    if (it == registers_.end()) {
      return nullptr;
    }
    return &it->second;
  }

private:
  std::unordered_map<std::uint32_t, RegisterDef> registers_;
  std::unordered_map<std::uint32_t, std::uint64_t> values_;
  RegisterCallback write_callback_;

  /// Apply write semantics based on access type.
  [[nodiscard]] std::uint64_t apply_write(const RegisterDef& register_definition,
                                          std::uint64_t old_value,
                                          std::uint64_t write_value) const noexcept;
};

/// Create default NIC register definitions for BAR0 (main register space).
inline std::vector<RegisterDef> MakeDefaultNicRegisters() {
  std::vector<RegisterDef> regs;

  // Device Control/Status registers
  regs.push_back(RegisterDef{
      .name = "CTRL",
      .offset = 0x0000,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RW,
      .reset_value = 0x00000000,
      .write_mask = 0xFFFFFFFF,
  });

  regs.push_back(RegisterDef{
      .name = "STATUS",
      .offset = 0x0008,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RO,
      .reset_value = 0x00000000,
      .write_mask = 0x00000000,
  });

  // Interrupt registers
  regs.push_back(RegisterDef{
      .name = "ICR",  // Interrupt Cause Read
      .offset = 0x00C0,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RC,  // Read-to-clear
      .reset_value = 0x00000000,
      .write_mask = 0x00000000,
  });

  regs.push_back(RegisterDef{
      .name = "ICS",  // Interrupt Cause Set
      .offset = 0x00C8,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::WO,
      .reset_value = 0x00000000,
      .write_mask = 0xFFFFFFFF,
  });

  regs.push_back(RegisterDef{
      .name = "IMS",  // Interrupt Mask Set
      .offset = 0x00D0,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RW1S,
      .reset_value = 0x00000000,
      .write_mask = 0xFFFFFFFF,
  });

  regs.push_back(RegisterDef{
      .name = "IMC",  // Interrupt Mask Clear
      .offset = 0x00D8,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::WO,
      .reset_value = 0x00000000,
      .write_mask = 0xFFFFFFFF,
  });

  // RX Control
  regs.push_back(RegisterDef{
      .name = "RCTL",
      .offset = 0x0100,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RW,
      .reset_value = 0x00000000,
      .write_mask = 0xFFFFFFFF,
  });

  // TX Control
  regs.push_back(RegisterDef{
      .name = "TCTL",
      .offset = 0x0400,
      .width = RegisterWidth::Bits32,
      .access = RegisterAccess::RW,
      .reset_value = 0x00000000,
      .write_mask = 0xFFFFFFFF,
  });

  return regs;
}

}  // namespace nic
