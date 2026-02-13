#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "nic/completion_queue.h"
#include "nic/msix.h"

namespace nic {

struct InterruptEvent {
  std::uint16_t queue_id{0};
  CompletionEntry completion{};
};

struct CoalesceConfig {
  std::uint32_t packet_threshold{1};
  std::uint32_t timer_threshold_us{0};  ///< Placeholder; timer not yet driven by real clock
};

struct AdaptiveConfig {
  bool enabled{false};
  std::uint32_t min_threshold{1};      ///< Minimum packet threshold
  std::uint32_t max_threshold{64};     ///< Maximum packet threshold
  std::uint32_t low_batch_size{4};     ///< Batch size below which we decrease threshold
  std::uint32_t high_batch_size{16};   ///< Batch size above which we increase threshold
  std::uint32_t sample_interval{100};  ///< Number of interrupts between adjustments
};

struct InterruptStats {
  std::uint64_t interrupts_fired{0};
  std::uint64_t coalesced_batches{0};
  std::uint64_t suppressed_masked{0};
  std::uint64_t suppressed_disabled{0};
  std::uint64_t manual_flushes{0};
  std::uint64_t timer_flushes{0};
  std::unordered_map<std::uint16_t, std::uint64_t> per_vector_fired{};
  std::unordered_map<std::uint16_t, std::uint64_t> per_vector_suppressed_masked{};
  std::unordered_map<std::uint16_t, std::uint64_t> per_vector_suppressed_disabled{};
};

/// Simple dispatcher that maps queue/admin events to MSI-X vectors and coalesces them.
class InterruptDispatcher {
public:
  using DeliverFn = std::function<void(std::uint16_t vector_id, std::uint32_t batch_size)>;

  InterruptDispatcher(MsixTable table,
                      MsixMapping mapping,
                      CoalesceConfig config,
                      DeliverFn deliver);

  bool on_completion(const InterruptEvent& ev);
  void flush(std::optional<std::uint16_t> vector_id = std::nullopt);
  void on_timer_tick(std::uint32_t elapsed_us);

  bool set_queue_vector(std::uint16_t queue_id, std::uint16_t vector_id) noexcept;
  bool mask_vector(std::uint16_t vector_id, bool masked = true) noexcept;
  bool enable_vector(std::uint16_t vector_id, bool enabled = true) noexcept;

  bool set_queue_coalesce_config(std::uint16_t queue_id, const CoalesceConfig& config) noexcept;
  [[nodiscard]] std::optional<CoalesceConfig> queue_coalesce_config(
      std::uint16_t queue_id) const noexcept;
  void clear_queue_coalesce_config(std::uint16_t queue_id) noexcept;

  void set_adaptive_config(const AdaptiveConfig& config) noexcept;
  [[nodiscard]] const AdaptiveConfig& adaptive_config() const noexcept { return adaptive_; }

  [[nodiscard]] const InterruptStats& stats() const noexcept { return stats_; }

private:
  struct AdaptiveState {
    std::uint32_t interrupt_count{0};    ///< Interrupts since last adjustment
    std::uint64_t total_batch_size{0};   ///< Sum of batch sizes in sample period
    std::uint32_t current_threshold{1};  ///< Current adaptive threshold
  };

  MsixTable table_;
  MsixMapping mapping_;
  CoalesceConfig coalesce_;
  AdaptiveConfig adaptive_{};
  DeliverFn deliver_;
  InterruptStats stats_{};
  std::unordered_map<std::uint16_t, std::uint32_t> pending_counts_;
  std::unordered_map<std::uint16_t, std::uint32_t> pending_time_us_;
  std::unordered_map<std::uint16_t, CoalesceConfig> per_queue_coalesce_;
  std::unordered_map<std::uint16_t, AdaptiveState> adaptive_state_;

  void try_fire(std::uint16_t vector_id);
  void update_adaptive_threshold(std::uint16_t vector_id, std::uint32_t batch_size) noexcept;
  [[nodiscard]] std::optional<std::uint16_t> resolve_vector(std::uint16_t queue_id) const noexcept;
  [[nodiscard]] const CoalesceConfig& get_coalesce_config(std::uint16_t queue_id) const noexcept;
};

}  // namespace nic
