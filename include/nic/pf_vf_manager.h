#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "nic/virtual_function.h"

namespace nic {

struct PFConfig {
  std::uint16_t max_vfs{64};
  std::uint16_t total_queues{128};
  std::uint16_t total_vectors{128};
  std::uint16_t pf_reserved_queues{8};
  std::uint16_t pf_reserved_vectors{8};
};

struct ResourceAllocation {
  std::uint16_t queue_start{0};
  std::uint16_t queue_count{0};
  std::uint16_t vector_start{0};
  std::uint16_t vector_count{0};
};

/// Manages Physical Function (PF) and Virtual Functions (VFs) for SR-IOV.
class PFVFManager {
public:
  explicit PFVFManager(PFConfig config);

  // VF lifecycle
  bool create_vf(std::uint16_t vf_id, const VFConfig& config);
  bool destroy_vf(std::uint16_t vf_id);
  bool enable_vf(std::uint16_t vf_id);
  bool disable_vf(std::uint16_t vf_id);
  bool reset_vf(std::uint16_t vf_id);

  // Resource queries
  [[nodiscard]] std::optional<ResourceAllocation> try_allocate_resources(
      std::uint16_t num_queues, std::uint16_t num_vectors) const noexcept;
  [[nodiscard]] bool has_available_resources(std::uint16_t num_queues,
                                             std::uint16_t num_vectors) const noexcept;

  // VF accessors
  [[nodiscard]] VirtualFunction* vf(std::uint16_t vf_id) noexcept;
  [[nodiscard]] const VirtualFunction* vf(std::uint16_t vf_id) const noexcept;
  [[nodiscard]] std::size_t num_active_vfs() const noexcept;
  [[nodiscard]] const PFConfig& config() const noexcept { return config_; }

  // Resource utilization
  [[nodiscard]] std::uint16_t available_queues() const noexcept;
  [[nodiscard]] std::uint16_t available_vectors() const noexcept;

private:
  PFConfig config_;
  std::unordered_map<std::uint16_t, std::unique_ptr<VirtualFunction>> vfs_;
  std::unordered_map<std::uint16_t, ResourceAllocation> vf_allocations_;
  std::vector<bool> queue_allocated_;   ///< Track allocated queues
  std::vector<bool> vector_allocated_;  ///< Track allocated vectors

  ResourceAllocation allocate_resources(std::uint16_t num_queues, std::uint16_t num_vectors);
  void free_resources(const ResourceAllocation& alloc);

  std::uint16_t find_free_queue_range(std::uint16_t count) const noexcept;
  std::uint16_t find_free_vector_range(std::uint16_t count) const noexcept;
  void mark_queues(std::uint16_t start, std::uint16_t count, bool allocated) noexcept;
  void mark_vectors(std::uint16_t start, std::uint16_t count, bool allocated) noexcept;
};

}  // namespace nic