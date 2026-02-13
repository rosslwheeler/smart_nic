#include "nic/msix.h"

#include "nic/trace.h"

using namespace nic;

MsixTable::MsixTable(std::size_t count) : vectors_(count) {
  NIC_TRACE_SCOPED(__func__);
}

bool MsixTable::set_vector(std::size_t idx, const MsixVector& vec) noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (!valid_index(idx)) {
    return false;
  }
  vectors_[idx] = vec;
  return true;
}

std::optional<MsixVector> MsixTable::vector(std::size_t idx) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (!valid_index(idx)) {
    return std::nullopt;
  }
  return vectors_[idx];
}

bool MsixTable::mask(std::size_t idx, bool masked) noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (!valid_index(idx)) {
    return false;
  }
  vectors_[idx].masked = masked;
  return true;
}

bool MsixTable::enable(std::size_t idx, bool enabled) noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (!valid_index(idx)) {
    return false;
  }
  vectors_[idx].enabled = enabled;
  return true;
}

MsixMapping::MsixMapping(std::size_t queue_count, std::uint16_t default_vector)
  : admin_vector_(default_vector),
    default_vector_(default_vector),
    queue_vectors_(queue_count, default_vector) {
  NIC_TRACE_SCOPED(__func__);
}

bool MsixMapping::set_queue_vector(std::size_t queue_id, std::uint16_t vector) noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (queue_id >= queue_vectors_.size()) {
    return false;
  }
  queue_vectors_[queue_id] = vector;
  return true;
}

std::uint16_t MsixMapping::queue_vector(std::size_t queue_id) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (queue_id >= queue_vectors_.size()) {
    return default_vector_;
  }
  return queue_vectors_[queue_id];
}
