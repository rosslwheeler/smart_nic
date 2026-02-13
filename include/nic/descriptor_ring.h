#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "nic/dma_engine.h"
#include "nic/doorbell.h"

namespace nic {

struct DescriptorRingConfig {
  std::size_t descriptor_size{0};
  std::size_t ring_size{0};
  HostAddress base_address{0};  // Host-backed base; ignored for in-model storage
  std::uint16_t queue_id{0};
  bool host_backed{false};
};

/// Descriptor ring with optional host-backed storage and doorbell notification.
class DescriptorRing {
public:
  DescriptorRing(DescriptorRingConfig config, Doorbell* doorbell = nullptr);
  DescriptorRing(DescriptorRingConfig config, DMAEngine& dma_engine, Doorbell* doorbell = nullptr);

  [[nodiscard]] bool is_full() const noexcept;
  [[nodiscard]] bool is_empty() const noexcept;
  [[nodiscard]] std::size_t available() const noexcept;
  [[nodiscard]] std::size_t space() const noexcept;

  /// Write a descriptor at the producer index and advance it.
  [[nodiscard]] DmaResult push_descriptor(std::span<const std::byte> descriptor);

  /// Read the descriptor at the consumer index and advance it.
  [[nodiscard]] DmaResult pop_descriptor(std::span<std::byte> descriptor);

  void reset();

  [[nodiscard]] std::uint32_t producer_index() const noexcept { return producer_index_; }
  [[nodiscard]] std::uint32_t consumer_index() const noexcept { return consumer_index_; }

private:
  DescriptorRingConfig config_{};
  Doorbell* doorbell_{nullptr};
  DMAEngine* dma_engine_{nullptr};
  std::vector<std::byte> storage_;
  std::uint32_t producer_index_{0};
  std::uint32_t consumer_index_{0};
  std::uint32_t count_{0};

  [[nodiscard]] HostAddress slot_address(std::uint32_t slot) const noexcept;
  [[nodiscard]] std::span<std::byte> slot_span(std::uint32_t slot) noexcept;
  [[nodiscard]] std::span<const std::byte> const_slot_span(std::uint32_t slot) const noexcept;
};

}  // namespace nic
