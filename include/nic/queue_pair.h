#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nic/completion_queue.h"
#include "nic/descriptor_ring.h"
#include "nic/dma_engine.h"
#include "nic/doorbell.h"
#include "nic/interrupt_dispatcher.h"
#include "nic/offload.h"
#include "nic/tx_rx.h"

namespace nic {

struct QueuePairConfig {
  std::uint16_t queue_id{0};
  DescriptorRingConfig tx_ring{};
  DescriptorRingConfig rx_ring{};
  CompletionQueueConfig tx_completion{};
  CompletionQueueConfig rx_completion{};
  Doorbell* tx_doorbell{nullptr};
  Doorbell* rx_doorbell{nullptr};
  Doorbell* tx_completion_doorbell{nullptr};
  Doorbell* rx_completion_doorbell{nullptr};
  InterruptDispatcher* interrupt_dispatcher{nullptr};
  std::uint8_t weight{1};            ///< Scheduler weight (>=1)
  std::size_t max_mtu{kJumboMtu};    ///< Maximum supported MTU
  bool enable_tx_interrupts{false};  ///< Fire interrupts on TX completions
  bool enable_rx_interrupts{true};   ///< Fire interrupts on RX completions
};

struct QueuePairStats {
  std::uint64_t tx_packets{0};
  std::uint64_t rx_packets{0};
  std::uint64_t tx_bytes{0};
  std::uint64_t rx_bytes{0};
  std::uint64_t drops_checksum{0};
  std::uint64_t drops_no_rx_desc{0};
  std::uint64_t drops_buffer_small{0};
  std::uint64_t drops_mtu_exceeded{0};
  std::uint64_t drops_invalid_mss{0};
  std::uint64_t drops_too_many_segments{0};
  std::uint64_t tx_tso_segments{0};
  std::uint64_t tx_gso_segments{0};
  std::uint64_t tx_vlan_insertions{0};
  std::uint64_t rx_vlan_strips{0};
  std::uint64_t rx_checksum_verified{0};
  std::uint64_t rx_gro_aggregated{0};
};

/// Aggregates TX/RX rings and completion queues for a single queue pair.
class QueuePair {
public:
  QueuePair(QueuePairConfig config, DMAEngine& dma_engine);

  QueuePair(const QueuePair&) = delete;
  QueuePair& operator=(const QueuePair&) = delete;

  [[nodiscard]] DescriptorRing& tx_ring() noexcept;
  [[nodiscard]] DescriptorRing& rx_ring() noexcept;
  [[nodiscard]] CompletionQueue& tx_completion() noexcept;
  [[nodiscard]] CompletionQueue& rx_completion() noexcept;
  [[nodiscard]] const DescriptorRing& tx_ring() const noexcept;
  [[nodiscard]] const DescriptorRing& rx_ring() const noexcept;
  [[nodiscard]] const CompletionQueue& tx_completion() const noexcept;
  [[nodiscard]] const CompletionQueue& rx_completion() const noexcept;

  /// Process a single TX descriptor and loop back to RX (returns true if work was done).
  bool process_once();

  [[nodiscard]] const QueuePairStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::string stats_summary() const;
  void reset_stats() noexcept { stats_ = QueuePairStats{}; }

  void reset();

private:
  QueuePairConfig config_{};
  DMAEngine& dma_engine_;
  std::unique_ptr<DescriptorRing> tx_ring_;
  std::unique_ptr<DescriptorRing> rx_ring_;
  std::unique_ptr<CompletionQueue> tx_completion_;
  std::unique_ptr<CompletionQueue> rx_completion_;
  QueuePairStats stats_;

  bool decode_tx_descriptor(std::span<const std::byte> bytes, TxDescriptor& out) const noexcept;
  bool decode_rx_descriptor(std::span<const std::byte> bytes, RxDescriptor& out) const noexcept;
  CompletionEntry make_completion(std::uint16_t descriptor_index,
                                  CompletionCode status) const noexcept;
  CompletionEntry make_tx_completion(const TxDescriptor& tx_desc,
                                     CompletionCode status,
                                     std::size_t segments_count,
                                     bool performed_tso,
                                     bool performed_gso) const noexcept;
  bool validate_mtu(const TxDescriptor& tx_desc, const std::vector<std::byte>& packet);
  std::optional<std::vector<std::vector<std::byte>>> build_segments(
      const TxDescriptor& tx_desc, const std::vector<std::byte>& packet);
  void finalize_tx_success(const TxDescriptor& tx_desc,
                           std::size_t total_segments,
                           std::size_t packet_bytes,
                           bool performed_tso,
                           bool performed_gso);
  bool process_segments(const TxDescriptor& tx_desc, const std::vector<std::byte>& packet);
  bool handle_rx_segment(std::vector<std::byte> segment,
                         const TxDescriptor& tx_desc,
                         RxDescriptor& rx_desc,
                         std::size_t total_segments,
                         bool performed_tso,
                         bool performed_gso);
  void fire_tx_interrupt(const CompletionEntry& entry) noexcept;
  void fire_rx_interrupt(const CompletionEntry& entry) noexcept;
};

}  // namespace nic
