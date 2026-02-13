#pragma once

#include <cstdint>

namespace nic {

/// PTP hardware clock with nanosecond resolution and drift modeling.
/// Implements IEEE 1588-style timestamping and clock adjustment.
class PTPClock {
public:
  struct Config {
    std::uint64_t frequency_hz{1'000'000'000};  ///< Clock frequency (1 GHz = 1ns resolution)
    double drift_ppb{0.0};                      ///< Initial drift in parts per billion
    bool enabled{true};                         ///< Clock enabled
  };

  PTPClock() : PTPClock(Config{}) {}
  explicit PTPClock(Config config);

  /// Read current time in nanoseconds
  [[nodiscard]] std::uint64_t read_time_ns() const noexcept { return counter_; }

  /// Adjust time by offset (positive = advance, negative = retard)
  void adjust_time(std::int64_t offset_ns) noexcept;

  /// Adjust frequency (PPB = parts per billion, positive = faster, negative = slower)
  void adjust_frequency(double ppb_adjustment) noexcept;

  /// Tick clock forward by elapsed time
  void tick(std::uint64_t elapsed_ns) noexcept;

  /// Reset clock to zero
  void reset() noexcept;

  /// Enable/disable clock
  void set_enabled(bool enabled) noexcept { config_.enabled = enabled; }
  [[nodiscard]] bool enabled() const noexcept { return config_.enabled; }

  /// Get current effective drift (base + adjustments)
  [[nodiscard]] double effective_drift_ppb() const noexcept;

  /// Get tick count
  [[nodiscard]] std::uint64_t tick_count() const noexcept { return tick_count_; }

  /// Get total time adjustment applied
  [[nodiscard]] std::int64_t total_time_adjustment_ns() const noexcept {
    return total_time_adjustment_;
  }

  /// Get total frequency adjustment applied
  [[nodiscard]] double total_frequency_adjustment_ppb() const noexcept {
    return frequency_adjustment_;
  }

private:
  Config config_;
  std::uint64_t counter_{0};               ///< Free-running nanosecond counter
  double frequency_adjustment_{0.0};       ///< Accumulated frequency adjustment (PPB)
  std::uint64_t tick_count_{0};            ///< Number of ticks processed
  std::int64_t total_time_adjustment_{0};  ///< Total time adjustment applied
};

}  // namespace nic