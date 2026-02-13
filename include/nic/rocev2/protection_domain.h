#pragma once

/// @file protection_domain.h
/// @brief Protection Domain management for RoCEv2.

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "nic/trace.h"

namespace nic::rocev2 {

/// Protection Domain - groups related QPs and MRs.
class ProtectionDomain {
public:
  explicit ProtectionDomain(std::uint32_t pd_handle) : handle_(pd_handle) {}

  [[nodiscard]] std::uint32_t handle() const noexcept { return handle_; }

private:
  std::uint32_t handle_;
};

/// Protection Domain table configuration.
struct PdTableConfig {
  std::size_t max_pds{1024};
};

/// Protection Domain table statistics.
struct PdTableStats {
  std::uint64_t allocations{0};
  std::uint64_t deallocations{0};
  std::uint64_t allocation_failures{0};
};

/// Protection Domain table - manages PD allocations.
class PdTable {
public:
  explicit PdTable(PdTableConfig config = {});

  /// Allocate a new protection domain.
  /// @return PD handle on success, std::nullopt if table is full.
  [[nodiscard]] std::optional<std::uint32_t> allocate();

  /// Deallocate a protection domain.
  /// @param pd_handle The PD handle to deallocate.
  /// @return true if successfully deallocated, false if handle not found.
  bool deallocate(std::uint32_t pd_handle);

  /// Check if a PD handle is valid.
  [[nodiscard]] bool is_valid(std::uint32_t pd_handle) const noexcept;

  /// Get a PD by handle.
  [[nodiscard]] ProtectionDomain* get(std::uint32_t pd_handle) noexcept;
  [[nodiscard]] const ProtectionDomain* get(std::uint32_t pd_handle) const noexcept;

  /// Get current count of allocated PDs.
  [[nodiscard]] std::size_t count() const noexcept { return pds_.size(); }

  /// Get statistics.
  [[nodiscard]] const PdTableStats& stats() const noexcept { return stats_; }

  /// Reset all PDs.
  void reset();

private:
  PdTableConfig config_;
  std::unordered_map<std::uint32_t, std::unique_ptr<ProtectionDomain>> pds_;
  std::uint32_t next_handle_{1};
  PdTableStats stats_;
};

}  // namespace nic::rocev2
