#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace nic {

/// Centralized statistics collection for NIC components
class StatsCollector {
public:
  /// Port-level statistics (aggregated from all queues)
  struct PortStats {
    std::uint64_t rx_bytes{0};
    std::uint64_t rx_packets{0};
    std::uint64_t rx_errors{0};
    std::uint64_t rx_dropped{0};
    std::uint64_t tx_bytes{0};
    std::uint64_t tx_packets{0};
    std::uint64_t tx_errors{0};
    std::uint64_t tx_dropped{0};
  };

  /// Per-queue statistics
  struct QueueStats {
    std::atomic<std::uint64_t> tx_bytes{0};
    std::atomic<std::uint64_t> tx_packets{0};
    std::atomic<std::uint64_t> tx_errors{0};
    std::atomic<std::uint64_t> rx_bytes{0};
    std::atomic<std::uint64_t> rx_packets{0};
    std::atomic<std::uint64_t> rx_errors{0};
  };

  /// Per-VF statistics
  struct VFStats {
    std::atomic<std::uint64_t> tx_bytes{0};
    std::atomic<std::uint64_t> tx_packets{0};
    std::atomic<std::uint64_t> rx_bytes{0};
    std::atomic<std::uint64_t> rx_packets{0};
    std::atomic<std::uint64_t> mailbox_messages{0};
  };

  /// Error types for categorization
  enum class ErrorType {
    TxDescriptorError,
    TxDMAError,
    TxChecksumError,
    RxDescriptorError,
    RxDMAError,
    RxChecksumError,
    RxDroppedFull,
  };

  StatsCollector();

  /// Record TX packet
  void record_tx_packet(std::uint16_t queue_id, std::uint64_t bytes) noexcept;

  /// Record RX packet
  void record_rx_packet(std::uint16_t queue_id, std::uint64_t bytes) noexcept;

  /// Record error
  void record_error(std::uint16_t queue_id, ErrorType type) noexcept;

  /// Record TX packet for specific VF
  void record_vf_tx_packet(std::uint16_t vf_id, std::uint64_t bytes) noexcept;

  /// Record RX packet for specific VF
  void record_vf_rx_packet(std::uint16_t vf_id, std::uint64_t bytes) noexcept;

  /// Record VF mailbox message
  void record_vf_mailbox_message(std::uint16_t vf_id) noexcept;

  /// Get port-level statistics (aggregated)
  [[nodiscard]] PortStats port_stats() const noexcept;

  /// Get per-queue statistics
  [[nodiscard]] const QueueStats& queue_stats(std::uint16_t queue_id) const noexcept;

  /// Get per-VF statistics
  [[nodiscard]] const VFStats& vf_stats(std::uint16_t vf_id) const noexcept;

  /// Reset all statistics
  void reset_all() noexcept;

  /// Reset specific queue statistics
  void reset_queue(std::uint16_t queue_id) noexcept;

  /// Reset specific VF statistics
  void reset_vf(std::uint16_t vf_id) noexcept;

private:
  std::unordered_map<std::uint16_t, QueueStats> queue_stats_;
  std::unordered_map<std::uint16_t, VFStats> vf_stats_;

  // Helper to get or create queue stats
  QueueStats& get_queue_stats(std::uint16_t queue_id) noexcept;
  VFStats& get_vf_stats(std::uint16_t vf_id) noexcept;
};

}  // namespace nic