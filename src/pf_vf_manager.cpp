#include "nic/pf_vf_manager.h"

#include "nic/trace.h"

using namespace nic;

PFVFManager::PFVFManager(PFConfig config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);

  // Initialize allocation tracking
  queue_allocated_.resize(config_.total_queues, false);
  vector_allocated_.resize(config_.total_vectors, false);

  // Reserve resources for PF
  mark_queues(0, config_.pf_reserved_queues, true);
  mark_vectors(0, config_.pf_reserved_vectors, true);
}

bool PFVFManager::create_vf(std::uint16_t vf_id, const VFConfig& config) {
  NIC_TRACE_SCOPED(__func__);

  // Check if VF already exists
  if (vfs_.contains(vf_id)) {
    return false;
  }

  // Check max VFs limit
  if (vfs_.size() >= config_.max_vfs) {
    return false;
  }

  // Try to allocate resources
  auto alloc_opt = try_allocate_resources(config.num_queues, config.num_vectors);
  if (!alloc_opt.has_value()) {
    return false;
  }

  // Allocate resources
  auto alloc = allocate_resources(config.num_queues, config.num_vectors);

  // Create VF
  auto vf = std::make_unique<VirtualFunction>(config);

  // Assign queue and vector IDs
  std::vector<std::uint16_t> queue_ids;
  queue_ids.reserve(alloc.queue_count);
  for (std::uint16_t i = 0; i < alloc.queue_count; ++i) {
    queue_ids.push_back(alloc.queue_start + i);
  }

  std::vector<std::uint16_t> vector_ids;
  vector_ids.reserve(alloc.vector_count);
  for (std::uint16_t i = 0; i < alloc.vector_count; ++i) {
    vector_ids.push_back(alloc.vector_start + i);
  }

  vf->set_queue_ids(std::move(queue_ids));
  vf->set_vector_ids(std::move(vector_ids));

  // Store allocation and VF
  vf_allocations_[vf_id] = alloc;
  vfs_[vf_id] = std::move(vf);

  return true;
}

bool PFVFManager::destroy_vf(std::uint16_t vf_id) {
  NIC_TRACE_SCOPED(__func__);

  auto it = vfs_.find(vf_id);
  if (it == vfs_.end()) {
    return false;
  }

  // Free resources
  auto alloc_it = vf_allocations_.find(vf_id);
  if (alloc_it != vf_allocations_.end()) {
    free_resources(alloc_it->second);
    vf_allocations_.erase(alloc_it);
  }

  // Remove VF
  vfs_.erase(it);
  return true;
}

bool PFVFManager::enable_vf(std::uint16_t vf_id) {
  NIC_TRACE_SCOPED(__func__);
  auto* vf_ptr = vf(vf_id);
  if (vf_ptr == nullptr) {
    return false;
  }
  return vf_ptr->enable();
}

bool PFVFManager::disable_vf(std::uint16_t vf_id) {
  NIC_TRACE_SCOPED(__func__);
  auto* vf_ptr = vf(vf_id);
  if (vf_ptr == nullptr) {
    return false;
  }
  return vf_ptr->disable();
}

bool PFVFManager::reset_vf(std::uint16_t vf_id) {
  NIC_TRACE_SCOPED(__func__);
  auto* vf_ptr = vf(vf_id);
  if (vf_ptr == nullptr) {
    return false;
  }
  return vf_ptr->reset();
}

std::optional<ResourceAllocation> PFVFManager::try_allocate_resources(
    std::uint16_t num_queues, std::uint16_t num_vectors) const noexcept {
  NIC_TRACE_SCOPED(__func__);

  // Check if we have enough resources
  std::uint16_t queue_start = find_free_queue_range(num_queues);
  if (queue_start == 0xFFFF) {
    return std::nullopt;
  }

  std::uint16_t vector_start = find_free_vector_range(num_vectors);
  if (vector_start == 0xFFFF) {
    return std::nullopt;
  }

  return ResourceAllocation{queue_start, num_queues, vector_start, num_vectors};
}

bool PFVFManager::has_available_resources(std::uint16_t num_queues,
                                          std::uint16_t num_vectors) const noexcept {
  return try_allocate_resources(num_queues, num_vectors).has_value();
}

VirtualFunction* PFVFManager::vf(std::uint16_t vf_id) noexcept {
  auto it = vfs_.find(vf_id);
  if (it != vfs_.end()) {
    return it->second.get();
  }
  return nullptr;
}

const VirtualFunction* PFVFManager::vf(std::uint16_t vf_id) const noexcept {
  auto it = vfs_.find(vf_id);
  if (it != vfs_.end()) {
    return it->second.get();
  }
  return nullptr;
}

std::size_t PFVFManager::num_active_vfs() const noexcept {
  std::size_t count = 0;
  for (const auto& [_, vf_ptr] : vfs_) {
    if (vf_ptr->state() == VFState::Enabled) {
      ++count;
    }
  }
  return count;
}

std::uint16_t PFVFManager::available_queues() const noexcept {
  std::uint16_t count = 0;
  for (bool allocated : queue_allocated_) {
    if (!allocated) {
      ++count;
    }
  }
  return count;
}

std::uint16_t PFVFManager::available_vectors() const noexcept {
  std::uint16_t count = 0;
  for (bool allocated : vector_allocated_) {
    if (!allocated) {
      ++count;
    }
  }
  return count;
}

ResourceAllocation PFVFManager::allocate_resources(std::uint16_t num_queues,
                                                   std::uint16_t num_vectors) {
  NIC_TRACE_SCOPED(__func__);

  std::uint16_t queue_start = find_free_queue_range(num_queues);
  std::uint16_t vector_start = find_free_vector_range(num_vectors);

  mark_queues(queue_start, num_queues, true);
  mark_vectors(vector_start, num_vectors, true);

  return ResourceAllocation{queue_start, num_queues, vector_start, num_vectors};
}

void PFVFManager::free_resources(const ResourceAllocation& alloc) {
  NIC_TRACE_SCOPED(__func__);
  mark_queues(alloc.queue_start, alloc.queue_count, false);
  mark_vectors(alloc.vector_start, alloc.vector_count, false);
}

std::uint16_t PFVFManager::find_free_queue_range(std::uint16_t count) const noexcept {
  if ((count == 0) || (count > queue_allocated_.size())) {
    return 0xFFFF;
  }

  for (std::uint16_t start = 0; start <= queue_allocated_.size() - count; ++start) {
    bool found = true;
    for (std::uint16_t i = 0; i < count; ++i) {
      if (queue_allocated_[start + i]) {
        found = false;
        break;
      }
    }
    if (found) {
      return start;
    }
  }
  return 0xFFFF;
}

std::uint16_t PFVFManager::find_free_vector_range(std::uint16_t count) const noexcept {
  if ((count == 0) || (count > vector_allocated_.size())) {
    return 0xFFFF;
  }

  for (std::uint16_t start = 0; start <= vector_allocated_.size() - count; ++start) {
    bool found = true;
    for (std::uint16_t i = 0; i < count; ++i) {
      if (vector_allocated_[start + i]) {
        found = false;
        break;
      }
    }
    if (found) {
      return start;
    }
  }
  return 0xFFFF;
}

void PFVFManager::mark_queues(std::uint16_t start, std::uint16_t count, bool allocated) noexcept {
  for (std::uint16_t i = 0; i < count; ++i) {
    if (start + i < queue_allocated_.size()) {
      queue_allocated_[start + i] = allocated;
    }
  }
}

void PFVFManager::mark_vectors(std::uint16_t start, std::uint16_t count, bool allocated) noexcept {
  for (std::uint16_t i = 0; i < count; ++i) {
    if (start + i < vector_allocated_.size()) {
      vector_allocated_[start + i] = allocated;
    }
  }
}
