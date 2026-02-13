#include "nic/rocev2/protection_domain.h"

#include "nic/trace.h"

namespace nic::rocev2 {

PdTable::PdTable(PdTableConfig config) : config_(config) {
  NIC_TRACE_SCOPED(__func__);
}

std::optional<std::uint32_t> PdTable::allocate() {
  NIC_TRACE_SCOPED(__func__);

  if (pds_.size() >= config_.max_pds) {
    ++stats_.allocation_failures;
    return std::nullopt;
  }

  std::uint32_t handle = next_handle_++;
  pds_.emplace(handle, std::make_unique<ProtectionDomain>(handle));
  ++stats_.allocations;

  return handle;
}

bool PdTable::deallocate(std::uint32_t pd_handle) {
  NIC_TRACE_SCOPED(__func__);

  auto iter = pds_.find(pd_handle);
  if (iter == pds_.end()) {
    return false;
  }

  pds_.erase(iter);
  ++stats_.deallocations;
  return true;
}

bool PdTable::is_valid(std::uint32_t pd_handle) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return pds_.find(pd_handle) != pds_.end();
}

ProtectionDomain* PdTable::get(std::uint32_t pd_handle) noexcept {
  NIC_TRACE_SCOPED(__func__);

  auto iter = pds_.find(pd_handle);
  if (iter == pds_.end()) {
    return nullptr;
  }
  return iter->second.get();
}

const ProtectionDomain* PdTable::get(std::uint32_t pd_handle) const noexcept {
  NIC_TRACE_SCOPED(__func__);

  auto iter = pds_.find(pd_handle);
  if (iter == pds_.end()) {
    return nullptr;
  }
  return iter->second.get();
}

void PdTable::reset() {
  NIC_TRACE_SCOPED(__func__);
  pds_.clear();
  next_handle_ = 1;
  stats_ = {};
}

}  // namespace nic::rocev2
