
#include "nic/ptp_clock.h"

#include <cassert>
#include <cmath>
#include <cstdint>

using namespace nic;

int main() {
  // Basic clock creation and reading
  PTPClock::Config cfg{.frequency_hz = 1'000'000'000, .drift_ppb = 0.0, .enabled = true};
  PTPClock clock{cfg};

  assert(clock.read_time_ns() == 0);
  assert(clock.enabled());
  assert(clock.tick_count() == 0);

  // Tick forward 1 second (1 billion nanoseconds)
  clock.tick(1'000'000'000);
  assert(clock.read_time_ns() == 1'000'000'000);
  assert(clock.tick_count() == 1);

  // Tick forward another 500 milliseconds
  clock.tick(500'000'000);
  assert(clock.read_time_ns() == 1'500'000'000);
  assert(clock.tick_count() == 2);

  // Time adjustment - advance clock
  clock.adjust_time(1'000'000);  // Advance by 1ms
  assert(clock.read_time_ns() == 1'501'000'000);
  assert(clock.total_time_adjustment_ns() == 1'000'000);

  // Time adjustment - retard clock
  clock.adjust_time(-500'000);  // Retard by 500us
  assert(clock.read_time_ns() == 1'500'500'000);
  assert(clock.total_time_adjustment_ns() == 500'000);

  // Reset clock
  clock.reset();
  assert(clock.read_time_ns() == 0);
  assert(clock.tick_count() == 0);
  assert(clock.total_time_adjustment_ns() == 0);

  // Test clock with drift
  PTPClock::Config drift_cfg{.frequency_hz = 1'000'000'000,
                             .drift_ppb = 100.0,  // 100 PPB fast
                             .enabled = true};
  PTPClock drift_clock{drift_cfg};

  // Tick 1 second - with 100 PPB drift, clock should advance by 1s + 100ns
  drift_clock.tick(1'000'000'000);
  std::uint64_t expected = 1'000'000'100;  // 1s + 100ns drift
  assert(drift_clock.read_time_ns() == expected);

  // Frequency adjustment - compensate for drift
  drift_clock.adjust_frequency(-100.0);  // Slow down by 100 PPB to cancel drift
  assert(std::abs(drift_clock.effective_drift_ppb()) < 0.001);  // Should be ~0

  // Tick another second - drift should be canceled
  drift_clock.tick(1'000'000'000);
  expected = 2'000'000'100;  // Previous value + 1s (no additional drift)
  assert(drift_clock.read_time_ns() == expected);

  // Test negative drift (slow clock)
  PTPClock::Config slow_cfg{.frequency_hz = 1'000'000'000,
                            .drift_ppb = -50.0,  // 50 PPB slow
                            .enabled = true};
  PTPClock slow_clock{slow_cfg};

  slow_clock.tick(1'000'000'000);
  expected = 999'999'950;  // 1s - 50ns drift
  assert(slow_clock.read_time_ns() == expected);

  // Test disabled clock
  PTPClock disabled_clock{cfg};
  disabled_clock.set_enabled(false);
  assert(!disabled_clock.enabled());

  disabled_clock.tick(1'000'000'000);
  assert(disabled_clock.read_time_ns() == 0);  // Should not advance

  disabled_clock.adjust_time(1'000'000);
  assert(disabled_clock.read_time_ns() == 0);  // Should not adjust

  // Re-enable and verify it works
  disabled_clock.set_enabled(true);
  disabled_clock.tick(100);
  assert(disabled_clock.read_time_ns() == 100);

  // Test large time adjustment
  PTPClock adj_clock{cfg};
  adj_clock.tick(5'000'000'000);  // 5 seconds
  assert(adj_clock.read_time_ns() == 5'000'000'000);

  adj_clock.adjust_time(1'000'000'000);  // Advance by 1 second
  assert(adj_clock.read_time_ns() == 6'000'000'000);

  adj_clock.adjust_time(-2'000'000'000);  // Retard by 2 seconds
  assert(adj_clock.read_time_ns() == 4'000'000'000);

  // Test negative adjustment that would go below zero
  PTPClock zero_clock{cfg};
  zero_clock.tick(100);
  zero_clock.adjust_time(-200);            // Try to go negative
  assert(zero_clock.read_time_ns() == 0);  // Should clamp to 0

  // Test frequency adjustment accumulation
  PTPClock freq_clock{cfg};
  assert(freq_clock.effective_drift_ppb() == 0.0);

  freq_clock.adjust_frequency(10.0);
  assert(freq_clock.effective_drift_ppb() == 10.0);

  freq_clock.adjust_frequency(5.0);
  assert(freq_clock.effective_drift_ppb() == 15.0);

  freq_clock.adjust_frequency(-20.0);
  assert(freq_clock.effective_drift_ppb() == -5.0);

  return 0;
}
