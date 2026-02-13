#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

#include "nic/ptp_clock.h"

namespace nic {

/// Manages packet timestamping for PTP and general precision timing.
class PTPTimestamper {
public:
  explicit PTPTimestamper(PTPClock& clock);

  /// Capture TX timestamp for a packet
  [[nodiscard]] std::uint64_t timestamp_tx_packet(std::uint16_t queue_id) noexcept;

  /// Capture RX timestamp for a packet
  [[nodiscard]] std::uint64_t timestamp_rx_packet(std::uint16_t queue_id) noexcept;

  /// Check if packet appears to be PTP (simple heuristic)
  [[nodiscard]] bool is_ptp_packet(std::span<const std::byte> packet) const noexcept;

  /// Enable/disable TX timestamping for a queue
  void enable_tx_timestamping(std::uint16_t queue_id, bool enable) noexcept;

  /// Enable/disable RX timestamping for a queue
  void enable_rx_timestamping(std::uint16_t queue_id, bool enable) noexcept;

  /// Check if TX timestamping is enabled for a queue
  [[nodiscard]] bool tx_timestamping_enabled(std::uint16_t queue_id) const noexcept;

  /// Check if RX timestamping is enabled for a queue
  [[nodiscard]] bool rx_timestamping_enabled(std::uint16_t queue_id) const noexcept;

  /// Statistics
  struct Stats {
    std::uint64_t tx_timestamps{0};
    std::uint64_t rx_timestamps{0};
    std::uint64_t ptp_packets_detected{0};
  };

  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = Stats{}; }

private:
  PTPClock& clock_;
  std::unordered_map<std::uint16_t, bool> tx_enabled_;
  std::unordered_map<std::uint16_t, bool> rx_enabled_;
  Stats stats_{};
};

}  // namespace nic