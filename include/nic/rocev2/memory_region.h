#pragma once

/// @file memory_region.h
/// @brief Memory Region management for RoCEv2.

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>

#include "nic/host_memory.h"
#include "nic/rocev2/types.h"
#include "nic/trace.h"

namespace nic::rocev2 {

/// Memory Region - registered memory for RDMA operations.
struct MemoryRegion {
  std::uint32_t lkey;           // Local key
  std::uint32_t rkey;           // Remote key
  HostAddress virtual_address;  // Base virtual address
  std::size_t length;           // Size in bytes
  std::uint32_t pd_handle;      // Protection domain
  AccessFlags access;           // Access permissions
  bool is_valid{true};          // Invalidation flag
};

/// Memory Region table configuration.
struct MrTableConfig {
  std::size_t max_mrs{4096};
};

/// Memory Region table statistics.
struct MrTableStats {
  std::uint64_t registrations{0};
  std::uint64_t deregistrations{0};
  std::uint64_t lkey_validations{0};
  std::uint64_t rkey_validations{0};
  std::uint64_t access_errors{0};
  std::uint64_t registration_failures{0};
};

/// Memory Region Table - manages MR registrations.
class MemoryRegionTable {
public:
  explicit MemoryRegionTable(MrTableConfig config = {});

  /// Register a memory region.
  /// @param pd_handle Protection domain handle.
  /// @param virtual_address Base address of the memory region.
  /// @param length Size in bytes.
  /// @param access Access permissions.
  /// @return lkey on success, std::nullopt on failure.
  [[nodiscard]] std::optional<std::uint32_t> register_mr(std::uint32_t pd_handle,
                                                         HostAddress virtual_address,
                                                         std::size_t length,
                                                         AccessFlags access);

  /// Deregister a memory region by lkey.
  /// @param lkey The local key of the MR to deregister.
  /// @return true if successfully deregistered.
  bool deregister_mr(std::uint32_t lkey);

  /// Validate lkey access for local operations.
  /// @param lkey Local key to validate.
  /// @param address Address within the MR.
  /// @param length Length of the access.
  /// @param is_write true if write access required.
  /// @return true if access is valid.
  [[nodiscard]] bool validate_lkey(std::uint32_t lkey,
                                   HostAddress address,
                                   std::size_t length,
                                   bool is_write) const;

  /// Validate rkey access for remote operations.
  /// @param rkey Remote key to validate.
  /// @param pd_handle Protection domain (must match MR's PD).
  /// @param address Address within the MR.
  /// @param length Length of the access.
  /// @param is_write true if write access required.
  /// @return true if access is valid.
  [[nodiscard]] bool validate_rkey(std::uint32_t rkey,
                                   std::uint32_t pd_handle,
                                   HostAddress address,
                                   std::size_t length,
                                   bool is_write) const;

  /// Get MR by lkey.
  [[nodiscard]] const MemoryRegion* get_by_lkey(std::uint32_t lkey) const noexcept;

  /// Get MR by rkey.
  [[nodiscard]] const MemoryRegion* get_by_rkey(std::uint32_t rkey) const noexcept;

  /// Get current count of registered MRs.
  [[nodiscard]] std::size_t count() const noexcept { return mrs_by_lkey_.size(); }

  /// Get statistics.
  [[nodiscard]] const MrTableStats& stats() const noexcept { return stats_; }

  /// Reset all MRs.
  void reset();

private:
  MrTableConfig config_;
  std::unordered_map<std::uint32_t, std::unique_ptr<MemoryRegion>> mrs_by_lkey_;
  std::unordered_map<std::uint32_t, MemoryRegion*> mrs_by_rkey_;
  std::uint32_t next_key_{0x100};  // Start above 0 to catch null key bugs
  mutable MrTableStats stats_;

  [[nodiscard]] std::uint32_t generate_key();

  [[nodiscard]] bool validate_access(const MemoryRegion* mr,
                                     HostAddress address,
                                     std::size_t length,
                                     bool is_write,
                                     bool is_remote) const;
};

}  // namespace nic::rocev2
