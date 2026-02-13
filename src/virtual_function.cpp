#include "nic/virtual_function.h"

#include "nic/trace.h"

using namespace nic;

VirtualFunction::VirtualFunction(VFConfig config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);
  queue_ids_.reserve(config_.num_queues);
  vector_ids_.reserve(config_.num_vectors);
}

bool VirtualFunction::enable() noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (state_ == VFState::Enabled) {
    return true;
  }
  if (state_ == VFState::FLRInProgress) {
    return false;
  }
  state_ = VFState::Enabled;
  config_.enabled = true;
  return true;
}

bool VirtualFunction::disable() noexcept {
  NIC_TRACE_SCOPED(__func__);
  if (state_ == VFState::Disabled) {
    return true;
  }
  if (state_ == VFState::FLRInProgress) {
    return false;
  }
  state_ = VFState::Disabled;
  config_.enabled = false;
  return true;
}

bool VirtualFunction::reset() noexcept {
  NIC_TRACE_SCOPED(__func__);
  state_ = VFState::FLRInProgress;
  stats_.resets += 1;

  // Perform reset: clear state, keep resources allocated
  // In real hardware, this would trigger FLR signaling

  state_ = VFState::Reset;
  config_.enabled = false;
  return true;
}

std::span<const std::uint16_t> VirtualFunction::queue_ids() const noexcept {
  return queue_ids_;
}

std::span<const std::uint16_t> VirtualFunction::vector_ids() const noexcept {
  return vector_ids_;
}

void VirtualFunction::set_queue_ids(std::vector<std::uint16_t> ids) noexcept {
  NIC_TRACE_SCOPED(__func__);
  queue_ids_ = std::move(ids);
}

void VirtualFunction::set_vector_ids(std::vector<std::uint16_t> ids) noexcept {
  NIC_TRACE_SCOPED(__func__);
  vector_ids_ = std::move(ids);
}

void VirtualFunction::record_tx_packet(std::uint64_t bytes) noexcept {
  stats_.tx_packets += 1;
  stats_.tx_bytes += bytes;
}

void VirtualFunction::record_rx_packet(std::uint64_t bytes) noexcept {
  stats_.rx_packets += 1;
  stats_.rx_bytes += bytes;
}

void VirtualFunction::record_tx_drop() noexcept {
  stats_.tx_drops += 1;
}

void VirtualFunction::record_rx_drop() noexcept {
  stats_.rx_drops += 1;
}

void VirtualFunction::record_mailbox_message() noexcept {
  stats_.mailbox_messages += 1;
}
