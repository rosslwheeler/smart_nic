#include "nic/register.h"

using namespace nic;

std::uint32_t RegisterFile::read32(std::uint32_t offset) const noexcept {
  auto it = values_.find(offset);
  if (it == values_.end()) {
    return 0xFFFFFFFF;  // Unmapped register
  }

  auto def_it = registers_.find(offset);
  if ((def_it != registers_.end()) && (def_it->second.access == RegisterAccess::WO)) {
    return 0;  // Write-only register reads as 0
  }

  return static_cast<std::uint32_t>(it->second & 0xFFFFFFFF);
}

std::uint64_t RegisterFile::read64(std::uint32_t offset) const noexcept {
  auto it = values_.find(offset);
  if (it == values_.end()) {
    return 0xFFFFFFFFFFFFFFFF;
  }

  auto def_it = registers_.find(offset);
  if ((def_it != registers_.end()) && (def_it->second.access == RegisterAccess::WO)) {
    return 0;
  }

  return it->second;
}

void RegisterFile::write32(std::uint32_t offset, std::uint32_t value) {
  auto def_it = registers_.find(offset);
  if (def_it == registers_.end()) {
    return;  // Unmapped register, ignore write
  }

  const auto& register_definition = def_it->second;
  if (register_definition.access == RegisterAccess::RO) {
    return;  // Read-only, ignore write
  }

  auto it = values_.find(offset);
  std::uint64_t old_value = register_definition.reset_value;
  if (it != values_.end()) {
    old_value = it->second;
  }
  std::uint64_t new_value = apply_write(register_definition, old_value, value);

  values_[offset] = new_value;

  // Invoke callback if registered
  if (write_callback_) {
    write_callback_(offset, old_value, new_value);
  }
}

void RegisterFile::write64(std::uint32_t offset, std::uint64_t value) {
  auto def_it = registers_.find(offset);
  if (def_it == registers_.end()) {
    return;
  }

  const auto& register_definition = def_it->second;
  if (register_definition.access == RegisterAccess::RO) {
    return;
  }

  auto it = values_.find(offset);
  std::uint64_t old_value = register_definition.reset_value;
  if (it != values_.end()) {
    old_value = it->second;
  }
  std::uint64_t new_value = apply_write(register_definition, old_value, value);

  values_[offset] = new_value;

  if (write_callback_) {
    write_callback_(offset, old_value, new_value);
  }
}

std::uint64_t RegisterFile::apply_write(const RegisterDef& register_definition,
                                        std::uint64_t old_value,
                                        std::uint64_t write_value) const noexcept {
  std::uint64_t masked = write_value & register_definition.write_mask;
  std::uint64_t preserved = old_value & ~register_definition.write_mask;

  switch (register_definition.access) {
    case RegisterAccess::RW:
    case RegisterAccess::WO:
      return preserved | masked;

    case RegisterAccess::RW1C:
      // Write 1 to clear: clear bits that are written as 1
      return old_value & ~masked;

    case RegisterAccess::RW1S:
      // Write 1 to set: set bits that are written as 1
      return old_value | masked;

    case RegisterAccess::RO:
    case RegisterAccess::RC:
      return old_value;  // No write effect

    default:
      return preserved | masked;
  }
}
