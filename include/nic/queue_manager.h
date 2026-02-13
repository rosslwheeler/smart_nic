#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nic/queue_pair.h"

namespace nic {

struct QueueManagerConfig {
  std::vector<QueuePairConfig> queue_configs;
};

struct QueueManagerStats {
  std::uint64_t total_tx_packets{0};
  std::uint64_t total_rx_packets{0};
  std::uint64_t total_tx_bytes{0};
  std::uint64_t total_rx_bytes{0};
  std::uint64_t total_drops_checksum{0};
  std::uint64_t total_drops_no_rx_desc{0};
  std::uint64_t total_drops_buffer_small{0};
  std::uint64_t total_tx_tso_segments{0};
  std::uint64_t total_tx_gso_segments{0};
  std::uint64_t total_tx_vlan_insertions{0};
  std::uint64_t total_rx_vlan_strips{0};
  std::uint64_t total_rx_checksum_verified{0};
  std::uint64_t total_rx_gro_aggregated{0};
  std::uint64_t scheduler_advances{0};
  std::uint64_t scheduler_skips{0};
};

/// Manages multiple queue pairs and aggregates stats.
class QueueManager {
public:
  QueueManager(QueueManagerConfig config, DMAEngine& dma_engine);

  QueueManager(const QueueManager&) = delete;
  QueueManager& operator=(const QueueManager&) = delete;

  [[nodiscard]] std::size_t queue_count() const noexcept { return queue_pairs_.size(); }

  [[nodiscard]] QueuePair* queue(std::size_t index) noexcept;
  [[nodiscard]] const QueuePair* queue(std::size_t index) const noexcept;

  [[nodiscard]] std::optional<QueuePairStats> queue_stats(std::size_t index) const noexcept;

  /// Process one descriptor across queues (simple round-robin).
  bool process_once();

  void reset();

  [[nodiscard]] QueueManagerStats stats() const;
  [[nodiscard]] std::string stats_summary() const;

private:
  QueueManagerConfig config_;
  DMAEngine& dma_engine_;
  std::vector<std::unique_ptr<QueuePair>> queue_pairs_;
  std::size_t scheduler_index_{0};
  std::size_t scheduler_credit_{0};
  std::vector<std::uint8_t> weights_;
  std::uint64_t scheduler_advances_{0};
  std::uint64_t scheduler_skips_{0};

  void aggregate_stats(QueueManagerStats& out) const;
};

}  // namespace nic
