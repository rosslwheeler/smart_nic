#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "nic/completion_queue.h"
#include "nic/descriptor_ring.h"
#include "nic/dma_engine.h"
#include "nic/doorbell.h"
#include "nic/interrupt_dispatcher.h"
#include "nic/queue_pair.h"
#include "nic/virtual_function.h"

namespace nic {

/// Represents a virtual NIC device interface presented to a VF.
/// Each VF sees this as a complete, isolated NIC with its own queues, doorbells, and registers.
class VFDevice {
public:
  struct Config {
    std::uint16_t vf_id{0};
    VirtualFunction* vf{nullptr};  ///< Associated VF (managed by PF)
    DMAEngine* dma_engine{nullptr};
    InterruptDispatcher* interrupt_dispatcher{nullptr};
    std::uint16_t num_queue_pairs{1};
    std::uint16_t queue_depth{64};
    std::uint16_t completion_queue_depth{128};
  };

  explicit VFDevice(Config config);

  // Queue pair management
  [[nodiscard]] std::size_t num_queue_pairs() const noexcept { return queue_pairs_.size(); }
  [[nodiscard]] QueuePair* queue_pair(std::size_t index) noexcept;
  [[nodiscard]] const QueuePair* queue_pair(std::size_t index) const noexcept;

  // Process TX/RX for a specific queue pair (returns true if work was done)
  bool process_queue_pair(std::size_t index);

  // Process all queue pairs (returns number of queue pairs that did work)
  std::size_t process_all();

  // Doorbell access (VF driver rings these)
  [[nodiscard]] Doorbell* tx_doorbell(std::size_t qp_index) noexcept;
  [[nodiscard]] Doorbell* rx_doorbell(std::size_t qp_index) noexcept;
  [[nodiscard]] Doorbell* tx_completion_doorbell(std::size_t qp_index) noexcept;
  [[nodiscard]] Doorbell* rx_completion_doorbell(std::size_t qp_index) noexcept;

  // Statistics
  struct Stats {
    std::uint64_t total_tx_packets{0};
    std::uint64_t total_rx_packets{0};
    std::uint64_t total_tx_bytes{0};
    std::uint64_t total_rx_bytes{0};
    std::uint64_t total_drops{0};
  };

  [[nodiscard]] Stats aggregate_stats() const noexcept;
  void reset_stats() noexcept;

  // Reset the VF device (called on VF FLR)
  void reset();

  [[nodiscard]] std::uint16_t vf_id() const noexcept { return config_.vf_id; }
  [[nodiscard]] VirtualFunction* vf() const noexcept { return config_.vf; }

private:
  Config config_;
  std::vector<std::unique_ptr<QueuePair>> queue_pairs_;
  std::vector<std::unique_ptr<Doorbell>> tx_doorbells_;
  std::vector<std::unique_ptr<Doorbell>> rx_doorbells_;
  std::vector<std::unique_ptr<Doorbell>> tx_completion_doorbells_;
  std::vector<std::unique_ptr<Doorbell>> rx_completion_doorbells_;

  void initialize_queue_pairs();
};

}  // namespace nic