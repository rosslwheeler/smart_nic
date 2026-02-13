#include "nic/rocev2/memory_region.h"

#include "nic/log.h"
#include "nic/trace.h"

namespace nic::rocev2 {

MemoryRegionTable::MemoryRegionTable(MrTableConfig config) : config_(config) {
  NIC_TRACE_SCOPED(__func__);
}

std::optional<std::uint32_t> MemoryRegionTable::register_mr(std::uint32_t pd_handle,
                                                            HostAddress virtual_address,
                                                            std::size_t length,
                                                            AccessFlags access) {
  NIC_TRACE_SCOPED(__func__);

  if (mrs_by_lkey_.size() >= config_.max_mrs) {
    ++stats_.registration_failures;
    NIC_LOGF_WARNING(
        "MR registration failed: table full ({}/{})", mrs_by_lkey_.size(), config_.max_mrs);
    return std::nullopt;
  }

  if (length == 0) {
    ++stats_.registration_failures;
    return std::nullopt;
  }

  std::uint32_t lkey = generate_key();
  std::uint32_t rkey = generate_key();

  auto mr = std::make_unique<MemoryRegion>();
  mr->lkey = lkey;
  mr->rkey = rkey;
  mr->virtual_address = virtual_address;
  mr->length = length;
  mr->pd_handle = pd_handle;
  mr->access = access;
  mr->is_valid = true;

  MemoryRegion* mr_ptr = mr.get();
  mrs_by_lkey_.emplace(lkey, std::move(mr));
  mrs_by_rkey_.emplace(rkey, mr_ptr);

  ++stats_.registrations;
  NIC_LOGF_INFO("MR registered: lkey={:#x} rkey={:#x} pd={} addr={:#x} len={}",
                lkey,
                rkey,
                pd_handle,
                virtual_address,
                length);
  return lkey;
}

bool MemoryRegionTable::deregister_mr(std::uint32_t lkey) {
  NIC_TRACE_SCOPED(__func__);

  auto iter = mrs_by_lkey_.find(lkey);
  if (iter == mrs_by_lkey_.end()) {
    return false;
  }

  // Remove from rkey map first
  std::uint32_t rkey = iter->second->rkey;
  mrs_by_rkey_.erase(rkey);

  // Remove from lkey map
  mrs_by_lkey_.erase(iter);

  ++stats_.deregistrations;
  return true;
}

bool MemoryRegionTable::validate_lkey(std::uint32_t lkey,
                                      HostAddress address,
                                      std::size_t length,
                                      bool is_write) const {
  NIC_TRACE_SCOPED(__func__);

  ++stats_.lkey_validations;

  const MemoryRegion* mr = get_by_lkey(lkey);
  return validate_access(mr, address, length, is_write, false);
}

bool MemoryRegionTable::validate_rkey(std::uint32_t rkey,
                                      std::uint32_t pd_handle,
                                      HostAddress address,
                                      std::size_t length,
                                      bool is_write) const {
  NIC_TRACE_SCOPED(__func__);

  ++stats_.rkey_validations;

  const MemoryRegion* mr = get_by_rkey(rkey);
  if (mr == nullptr) {
    ++stats_.access_errors;
    return false;
  }

  // Check PD match
  if (mr->pd_handle != pd_handle) {
    ++stats_.access_errors;
    return false;
  }

  return validate_access(mr, address, length, is_write, true);
}

const MemoryRegion* MemoryRegionTable::get_by_lkey(std::uint32_t lkey) const noexcept {
  NIC_TRACE_SCOPED(__func__);

  auto iter = mrs_by_lkey_.find(lkey);
  if (iter == mrs_by_lkey_.end()) {
    return nullptr;
  }
  return iter->second.get();
}

const MemoryRegion* MemoryRegionTable::get_by_rkey(std::uint32_t rkey) const noexcept {
  NIC_TRACE_SCOPED(__func__);

  auto iter = mrs_by_rkey_.find(rkey);
  if (iter == mrs_by_rkey_.end()) {
    return nullptr;
  }
  return iter->second;
}

void MemoryRegionTable::reset() {
  NIC_TRACE_SCOPED(__func__);

  mrs_by_rkey_.clear();
  mrs_by_lkey_.clear();
  next_key_ = 0x100;
  stats_ = {};
}

std::uint32_t MemoryRegionTable::generate_key() {
  NIC_TRACE_SCOPED(__func__);
  return next_key_++;
}

bool MemoryRegionTable::validate_access(const MemoryRegion* mr,
                                        HostAddress address,
                                        std::size_t length,
                                        bool is_write,
                                        bool is_remote) const {
  NIC_TRACE_SCOPED(__func__);

  if (mr == nullptr) {
    ++stats_.access_errors;
    return false;
  }

  if (!mr->is_valid) {
    ++stats_.access_errors;
    return false;
  }

  // Check address bounds
  if (address < mr->virtual_address) {
    ++stats_.access_errors;
    return false;
  }

  std::uint64_t offset = address - mr->virtual_address;
  if (offset + length > mr->length) {
    ++stats_.access_errors;
    return false;
  }

  // Check access permissions
  if (is_remote) {
    if (is_write && !mr->access.remote_write) {
      ++stats_.access_errors;
      return false;
    }
    if (!is_write && !mr->access.remote_read) {
      ++stats_.access_errors;
      return false;
    }
  } else {
    if (is_write && !mr->access.local_write) {
      ++stats_.access_errors;
      return false;
    }
    if (!is_write && !mr->access.local_read) {
      ++stats_.access_errors;
      return false;
    }
  }

  return true;
}

}  // namespace nic::rocev2
