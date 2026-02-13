#include "nic/descriptor_ring.h"

#include <cstring>

#include "nic/log.h"
#include "nic/trace.h"

using namespace nic;

DescriptorRing::DescriptorRing(DescriptorRingConfig config, Doorbell* doorbell)
  : config_(config), doorbell_(doorbell) {
  NIC_TRACE_SCOPED(__func__);
  if (!config_.host_backed) {
    storage_.resize(config_.descriptor_size * config_.ring_size);
  }
}

DescriptorRing::DescriptorRing(DescriptorRingConfig config,
                               DMAEngine& dma_engine,
                               Doorbell* doorbell)
  : config_(config), doorbell_(doorbell), dma_engine_(&dma_engine) {
  NIC_TRACE_SCOPED(__func__);
  if (!config_.host_backed) {
    storage_.resize(config_.descriptor_size * config_.ring_size);
  }
}

bool DescriptorRing::is_full() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return count_ == config_.ring_size;
}

bool DescriptorRing::is_empty() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return count_ == 0;
}

std::size_t DescriptorRing::available() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return count_;
}

std::size_t DescriptorRing::space() const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return config_.ring_size - count_;
}

DmaResult DescriptorRing::push_descriptor(std::span<const std::byte> descriptor) {
  NIC_TRACE_SCOPED(__func__);
  if (descriptor.size() != config_.descriptor_size) {
    trace_dma_error(DmaError::AccessError, "descriptor_push_size_mismatch");
    return {DmaError::AccessError, 0, "size_mismatch"};
  }
  if (is_full()) {
    trace_dma_error(DmaError::AccessError, "descriptor_ring_full");
    NIC_LOGF_DEBUG("descriptor ring full: count={}/{}", count_, config_.ring_size);
    return {DmaError::AccessError, 0, "ring_full"};
  }

  if (config_.host_backed) {
    if (dma_engine_ == nullptr) {
      trace_dma_error(DmaError::InternalError, "descriptor_ring_no_dma");
      return {DmaError::InternalError, 0, "no_dma"};
    }
    HostAddress address = slot_address(producer_index_);
    DmaResult result = dma_engine_->write(address, descriptor);
    if (!result.ok()) {
      return result;
    }
  } else {
    auto dest = slot_span(producer_index_);
    std::memcpy(dest.data(), descriptor.data(), descriptor.size());
  }

  producer_index_ = (producer_index_ + 1) % config_.ring_size;
  ++count_;

  if (doorbell_ != nullptr) {
    doorbell_->ring(DoorbellPayload{config_.queue_id, producer_index_});
  }

  return {DmaError::None, descriptor.size(), nullptr};
}

DmaResult DescriptorRing::pop_descriptor(std::span<std::byte> descriptor) {
  NIC_TRACE_SCOPED(__func__);
  if (descriptor.size() != config_.descriptor_size) {
    trace_dma_error(DmaError::AccessError, "descriptor_pop_size_mismatch");
    return {DmaError::AccessError, 0, "size_mismatch"};
  }
  if (is_empty()) {
    trace_dma_error(DmaError::AccessError, "descriptor_ring_empty");
    NIC_LOGF_DEBUG("descriptor ring empty");
    return {DmaError::AccessError, 0, "ring_empty"};
  }

  if (config_.host_backed) {
    if (dma_engine_ == nullptr) {
      trace_dma_error(DmaError::InternalError, "descriptor_ring_no_dma");
      return {DmaError::InternalError, 0, "no_dma"};
    }
    HostAddress address = slot_address(consumer_index_);
    DmaResult result = dma_engine_->read(address, descriptor);
    if (!result.ok()) {
      return result;
    }
  } else {
    auto src = const_slot_span(consumer_index_);
    std::memcpy(descriptor.data(), src.data(), descriptor.size());
  }

  consumer_index_ = (consumer_index_ + 1) % config_.ring_size;
  --count_;
  return {DmaError::None, descriptor.size(), nullptr};
}

void DescriptorRing::reset() {
  NIC_TRACE_SCOPED(__func__);
  producer_index_ = 0;
  consumer_index_ = 0;
  count_ = 0;
}

HostAddress DescriptorRing::slot_address(std::uint32_t slot) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  return config_.base_address + static_cast<HostAddress>(slot * config_.descriptor_size);
}

std::span<std::byte> DescriptorRing::slot_span(std::uint32_t slot) noexcept {
  NIC_TRACE_SCOPED(__func__);
  std::size_t offset = static_cast<std::size_t>(slot) * config_.descriptor_size;
  return std::span<std::byte>{storage_.data() + offset, config_.descriptor_size};
}

std::span<const std::byte> DescriptorRing::const_slot_span(std::uint32_t slot) const noexcept {
  NIC_TRACE_SCOPED(__func__);
  std::size_t offset = static_cast<std::size_t>(slot) * config_.descriptor_size;
  return std::span<const std::byte>{storage_.data() + offset, config_.descriptor_size};
}
