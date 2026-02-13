#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace nic {

struct VFConfig {
  std::uint16_t vf_id{0};
  std::uint16_t num_queues{1};
  std::uint16_t num_vectors{1};
  bool enabled{false};
  bool trust{false};  ///< Trusted VF can change MAC, VLAN, etc.
};

enum class VFState {
  Disabled,
  Enabled,
  Reset,
  FLRInProgress  ///< Function Level Reset
};

struct VFStats {
  std::uint64_t tx_packets{0};
  std::uint64_t rx_packets{0};
  std::uint64_t tx_bytes{0};
  std::uint64_t rx_bytes{0};
  std::uint64_t tx_drops{0};
  std::uint64_t rx_drops{0};
  std::uint64_t resets{0};
  std::uint64_t mailbox_messages{0};
};

/// Represents a Virtual Function (VF) in SR-IOV architecture.
class VirtualFunction {
public:
  explicit VirtualFunction(VFConfig config);

  bool enable() noexcept;
  bool disable() noexcept;
  bool reset() noexcept;

  [[nodiscard]] VFState state() const noexcept { return state_; }
  [[nodiscard]] const VFConfig& config() const noexcept { return config_; }
  [[nodiscard]] const VFStats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept { stats_ = VFStats{}; }

  // Resource accessors
  [[nodiscard]] std::span<const std::uint16_t> queue_ids() const noexcept;
  [[nodiscard]] std::span<const std::uint16_t> vector_ids() const noexcept;

  void set_queue_ids(std::vector<std::uint16_t> ids) noexcept;
  void set_vector_ids(std::vector<std::uint16_t> ids) noexcept;

  // Statistics updates (called by data plane)
  void record_tx_packet(std::uint64_t bytes) noexcept;
  void record_rx_packet(std::uint64_t bytes) noexcept;
  void record_tx_drop() noexcept;
  void record_rx_drop() noexcept;
  void record_mailbox_message() noexcept;

private:
  VFConfig config_;
  VFState state_{VFState::Disabled};
  VFStats stats_{};
  std::vector<std::uint16_t> queue_ids_;
  std::vector<std::uint16_t> vector_ids_;
};

}  // namespace nic