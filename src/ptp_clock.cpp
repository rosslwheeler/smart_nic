#include "nic/ptp_clock.h"

#include <utility>

#include "nic/trace.h"

using namespace nic;

PTPClock::PTPClock(Config config) : config_(std::move(config)) {
  NIC_TRACE_SCOPED(__func__);
}

void PTPClock::adjust_time(std::int64_t offset_ns) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return;
  }

  // Apply time offset (can be positive or negative)
  if (offset_ns >= 0) {
    counter_ += static_cast<std::uint64_t>(offset_ns);
  } else {
    std::uint64_t decrement = static_cast<std::uint64_t>(-offset_ns);
    if (decrement > counter_) {
      counter_ = 0;  // Don't go negative
    } else {
      counter_ -= decrement;
    }
  }

  total_time_adjustment_ += offset_ns;
}

void PTPClock::adjust_frequency(double ppb_adjustment) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return;
  }

  frequency_adjustment_ += ppb_adjustment;
}

void PTPClock::tick(std::uint64_t elapsed_ns) noexcept {
  NIC_TRACE_SCOPED(__func__);

  if (!config_.enabled) {
    return;
  }

  // Calculate effective drift (base drift + frequency adjustments)
  double effective_ppb = config_.drift_ppb + frequency_adjustment_;

  // Apply drift: PPB means parts per billion
  // If drift is 100 PPB and we tick 1 second (1e9 ns), clock advances by 1e9 + 100 ns
  double drift_factor = 1.0 + (effective_ppb / 1'000'000'000.0);
  std::uint64_t adjusted_elapsed = static_cast<std::uint64_t>(elapsed_ns * drift_factor);

  counter_ += adjusted_elapsed;
  tick_count_ += 1;
}

void PTPClock::reset() noexcept {
  NIC_TRACE_SCOPED(__func__);
  counter_ = 0;
  frequency_adjustment_ = 0.0;
  tick_count_ = 0;
  total_time_adjustment_ = 0;
}

double PTPClock::effective_drift_ppb() const noexcept {
  return config_.drift_ppb + frequency_adjustment_;
}
